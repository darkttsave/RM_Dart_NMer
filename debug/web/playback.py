#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
M2 离线回放服务（独立 Flask 进程）

职责：读取 C++ 视觉程序录制的会话目录，在浏览器中回放视频、遥测与曲线。
特点：
- 不连接工业相机、不连接电控、不需要启动 C++ 主程序；
- 会话目录可复制到另一台电脑后直接打开（不依赖原始绝对路径）；
- 时间关联以 video_index_001.csv 的 source_elapsed_us 为准，不使用视频文件标称 FPS；
- 只读：不驱动任何视觉/控制链路。

启动：
    python3 debug/web/playback.py --dir recordings/2026-11-21_10-31-51 --port 5001
    python3 debug/web/playback.py --root recordings --port 5001      # 从会话列表选择

依赖：Python 3、Flask、OpenCV（python3-opencv / opencv-python）
"""

import argparse
import csv
import json
import os
import threading
import time
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor

from flask import Flask, Response, jsonify, render_template, request

try:
    import cv2
except ImportError as error:  # pragma: no cover
    print(f"[playback] 错误: 缺少 OpenCV（{error}）。请安装 python3-opencv 或 opencv-python。")
    raise

app = Flask(__name__, static_folder="static", template_folder="templates")
app.config["TEMPLATES_AUTO_RELOAD"] = True
app.jinja_env.auto_reload = True

# 启动参数
CONFIG = {"root": "", "single_dir": "", "live_url": ""}


# ---------------------------------------------------------------------------
# 会话读取
# ---------------------------------------------------------------------------
def _read_csv(path):
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def _to_float(value, default=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _to_int(value, default=0):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return default


class VideoReader:
    """按帧索引读取 AVI，顺序读 + JPEG 缓存（只读）

    注意：OpenCV 的 cv2.VideoCapture 不是线程安全的，且同一实例跨线程调用在
    Flask 多线程请求下会出现偶发解码失败（M2 实测）。因此这里把所有 OpenCV
    解码/编码调用固定到一个单线程执行器（ThreadPoolExecutor(max_workers=1)），
    外部请求无论来自哪个 Flask 线程，实际视频 IO 都串行在该专用线程中完成。
    """

    CACHE_MAX = 180

    def __init__(self, path):
        self.path = path
        self.cap = None
        self.pos = -1
        self.cache = OrderedDict()
        self.frame_count = 0
        # 单线程执行器：所有 cv2.* 调用都在同一个 worker 线程内执行
        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="video-decoder")
        if os.path.isfile(path):
            try:
                self.frame_count = self._executor.submit(self._probe).result(timeout=10)
            except Exception:  # noqa: BLE001
                self.frame_count = 0

    def _probe(self):
        cap = cv2.VideoCapture(self.path)
        try:
            return int(cap.get(cv2.CAP_PROP_FRAME_COUNT)) if cap.isOpened() else 0
        finally:
            cap.release()

    def _open(self):
        if self.cap is not None:
            try:
                self.cap.release()
            except Exception:  # noqa: BLE001
                pass
        self.cap = cv2.VideoCapture(self.path)
        self.pos = -1
        return self.cap.isOpened()

    def _decode(self, index):
        """在专用线程内执行：顺序读并编码为 JPEG"""
        if index in self.cache:
            self.cache.move_to_end(index)
            return self.cache[index]
        if self.cap is None or not self.cap.isOpened() or index < self.pos:
            if not self._open():
                return None
        while self.pos < index:
            ok, frame = self.cap.read()
            if not ok or frame is None:
                return None
            self.pos += 1
            if self.pos == index:
                ok2, jpeg = cv2.imencode(".jpg", frame, [int(cv2.IMWRITE_JPEG_QUALITY), 85])
                if not ok2:
                    return None
                data = jpeg.tobytes()
                self.cache[self.pos] = data
                while len(self.cache) > self.CACHE_MAX:
                    self.cache.popitem(last=False)
                return data
        return self.cache.get(index)

    def get_jpeg(self, index):
        index = int(index)
        if index < 0:
            return None
        try:
            # 单线程执行器：保证 cv2 调用串行且始终在同一个 worker 线程中
            return self._executor.submit(self._decode, index).result(timeout=15)
        except Exception:  # noqa: BLE001
            return None


class ReplaySession:
    def __init__(self, session_dir):
        self.dir = os.path.abspath(session_dir)
        self.meta = {}
        meta_path = os.path.join(self.dir, "session.json")
        if os.path.isfile(meta_path):
            try:
                with open(meta_path, "r", encoding="utf-8") as handle:
                    self.meta = json.load(handle)
            except (OSError, json.JSONDecodeError):
                self.meta = {}

        self.telemetry = []
        telemetry_path = os.path.join(self.dir, "telemetry_001.csv")
        if os.path.isfile(telemetry_path):
            for row in _read_csv(telemetry_path):
                self.telemetry.append({
                    "frame_id": _to_int(row.get("frame_id")),
                    "timestamp_us": _to_int(row.get("timestamp_us")),
                    "elapsed_ms": _to_int(row.get("elapsed_us")) / 1000.0,
                    "detected": _to_int(row.get("detected")) == 1,
                    "tracking": _to_int(row.get("tracking")) == 1,
                    "target_x": _to_float(row.get("target_x")),
                    "target_y": _to_float(row.get("target_y")),
                    "radius": _to_float(row.get("radius")),
                    "image_width": _to_int(row.get("image_width")),
                    "image_height": _to_int(row.get("image_height")),
                    "image_center_x": _to_float(row.get("image_center_x")),
                    "image_center_y": _to_float(row.get("image_center_y")),
                    "offset_x": _to_float(row.get("offset_x")),
                    "offset_y": _to_float(row.get("offset_y")),
                    "turn_flag": _to_float(row.get("turn_flag")),
                    "distance_flag": _to_float(row.get("distance_flag")),
                    "clear_flag": (None if row.get("clear_flag", "") == ""
                                   else _to_float(row.get("clear_flag"))),
                    "tx_sent": _to_int(row.get("tx_sent")) == 1,
                    "tx_turn_flag": _to_float(row.get("tx_turn_flag")),
                    "tx_distance_flag": _to_float(row.get("tx_distance_flag")),
                    "tx_clear_flag": _to_float(row.get("tx_clear_flag")),
                    "lock_frames": _to_int(row.get("lock_frames")),
                    "required_frames": _to_int(row.get("required_frames")),
                    "lost_frames": _to_int(row.get("lost_frames")),
                    "required_lost_frames": _to_int(row.get("required_lost_frames")),
                    "green_pixels": _to_int(row.get("green_pixels")),
                    "candidate_count": _to_int(row.get("candidate_count")),
                    "fps": _to_float(row.get("fps")),
                    "processing_time_ms": _to_float(row.get("processing_time_ms")),
                })

        self.video_index = []
        index_path = os.path.join(self.dir, "video_index_001.csv")
        if os.path.isfile(index_path):
            for row in _read_csv(index_path):
                self.video_index.append({
                    "video_frame_index": _to_int(row.get("video_frame_index")),
                    "source_frame_id": _to_int(row.get("source_frame_id")),
                    "source_timestamp_us": _to_int(row.get("source_timestamp_us")),
                    "elapsed_ms": _to_int(row.get("source_elapsed_us")) / 1000.0,
                })

        # M3：参数修改事件（录制期间由 C++ 写入；无事件文件时保持空列表，不影响回放）
        self.events = []
        events_path = os.path.join(self.dir, "events.jsonl")
        if os.path.isfile(events_path):
            try:
                with open(events_path, "r", encoding="utf-8") as handle:
                    for line in handle:
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            item = json.loads(line)
                        except json.JSONDecodeError:
                            continue
                        self.events.append({
                            "index": len(self.events),
                            "type": str(item.get("type", "parameter_changed")),
                            "elapsed_ms": _to_float(item.get("elapsed_us"), 0.0) / 1000.0,
                            "frame_id": _to_int(item.get("frame_id"), 0),
                            "source": str(item.get("source", "")),
                            "parameter": str(item.get("parameter", "")),
                            "note": str(item.get("note", "")),
                            "old_value": _to_float(item.get("old_value"), 0.0),
                            "new_value": _to_float(item.get("new_value"), 0.0),
                            "result": str(item.get("result", "")),
                        })
            except OSError:
                self.events = []

        self.video_path = ""
        mode = self.meta.get("record_image_mode", "debug")
        video_meta = self.meta.get("video")
        metadata_file = video_meta.get("file") if isinstance(video_meta, dict) else None
        allowed = ("raw_001.avi",) if mode == "raw" else ("debug_001.avi", "debug.avi")
        names = ([metadata_file] if metadata_file in allowed else []) + list(allowed)
        for name in names:
            candidate = os.path.join(self.dir, name)
            if os.path.isfile(candidate):
                self.video_path = candidate
                break
        self.video = VideoReader(self.video_path) if self.video_path else None

        # 配置快照（M2-UI 页签 C「会话信息」只读展示用；不参与时间关联）
        self.config_snapshot = self._read_config_snapshot()

        # 时间轴范围
        last_t = 0.0
        if self.telemetry:
            last_t = self.telemetry[-1]["elapsed_ms"]
        if not last_t and self.meta.get("duration_s"):
            last_t = float(self.meta.get("duration_s")) * 1000.0
        self.duration_ms = max(last_t, 1.0)

        # 缺失视频帧统计（遥测有该帧，但视频未成功写入）
        telemetry_ids = {row["frame_id"] for row in self.telemetry}
        video_ids = {row["source_frame_id"] for row in self.video_index}
        self.missing_frame_ids = sorted(telemetry_ids - video_ids)
        self.video_elapsed = [row["elapsed_ms"] for row in self.video_index]

        # 视频帧中是否存在时间缺口（相邻 video 帧对应算法帧号差 > 1）
        gaps = []
        prev = None
        for row in self.video_index:
            if prev is not None and row["source_frame_id"] - prev > 1:
                gaps.append({
                    "after_video_index": row["video_frame_index"] - 1,
                    "missing_source_frames": row["source_frame_id"] - prev - 1,
                })
            prev = row["source_frame_id"]
        self.video_gaps = gaps

        self.complete = self.meta.get("status") == "completed"

    def _read_config_snapshot(self):
        """读取 config_snapshot/ 下的运行时参数快照（只读）

        页签 C 需要展示「录制时实际生效的参数快照」：session.json 只记录快照文件
        清单，参数内容在 config_snapshot/runtime_params.json 中，因此由回放服务
        读取后随 summary 返回。仅展示用途：不写入、不参与时间关联与视频定位。
        """
        snapshot = {"files": [], "params": None, "note": ""}
        snapshot_dir = os.path.join(self.dir, "config_snapshot")
        if not os.path.isdir(snapshot_dir):
            return snapshot
        try:
            snapshot["files"] = sorted(
                name for name in os.listdir(snapshot_dir)
                if os.path.isfile(os.path.join(snapshot_dir, name))
            )
        except OSError:
            return snapshot
        params_path = os.path.join(snapshot_dir, "runtime_params.json")
        if os.path.isfile(params_path):
            try:
                with open(params_path, "r", encoding="utf-8") as handle:
                    payload = json.load(handle)
            except (OSError, json.JSONDecodeError):
                payload = {}
            if isinstance(payload, dict):
                params = payload.get("params")
                if isinstance(params, dict):
                    snapshot["params"] = params
                note = payload.get("note")
                if isinstance(note, str):
                    snapshot["note"] = note
        return snapshot

    # ---- 时间 -> 视频帧索引（按真实时间戳，不使用标称 FPS）----
    def video_index_at(self, t_ms):
        if not self.video_index:
            return None
        # 二分查找最后一个 elapsed_ms <= t_ms
        lo, hi = 0, len(self.video_index) - 1
        if t_ms < self.video_index[0]["elapsed_ms"]:
            return 0
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.video_index[mid]["elapsed_ms"] <= t_ms:
                lo = mid
            else:
                hi = mid - 1
        return lo

    # ---- 时间 -> 遥测行（二分）----
    def telemetry_at(self, t_ms):
        if not self.telemetry:
            return None
        lo, hi = 0, len(self.telemetry) - 1
        if t_ms < self.telemetry[0]["elapsed_ms"]:
            return self.telemetry[0]
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if self.telemetry[mid]["elapsed_ms"] <= t_ms:
                lo = mid
            else:
                hi = mid - 1
        return self.telemetry[lo]

    def frame_id_at(self, frame_id):
        for row in self.telemetry:
            if row["frame_id"] == frame_id:
                return row
        return None

    def summary(self):
        counts = self.meta.get("counts", {})
        # 时间关联精度：只有同时存在遥测与逐帧视频索引，才能按真实时间戳精确关联
        accurate = bool(self.telemetry) and bool(self.video_index)
        if accurate:
            accuracy_note = (
                "按 video_index_001.csv 的 source_elapsed_us 逐帧索引关联；"
                "不使用视频标称 FPS，定位精度为相邻算法帧间隔（约 33 ms）"
            )
        elif self.telemetry:
            accurate = False
            accuracy_note = "该会话没有视频索引，只能按遥测时间浏览（无视频帧）"
        else:
            accuracy_note = "该会话没有遥测数据"
        return {
            "session": os.path.basename(self.dir),
            "dir": self.dir,
            "status": self.meta.get("status", "unknown"),
            "complete": self.complete,
            "started_at_wall": self.meta.get("started_at_wall", ""),
            "ended_at_wall": self.meta.get("ended_at_wall", ""),
            "duration_ms": self.duration_ms,
            "image_source": self.meta.get("image_source", {}),
            "image_size": self.meta.get("image_size", {}),
            "counts": counts,
            "telemetry_rows": len(self.telemetry),
            "video_frames_indexed": len(self.video_index),
            "missing_video_frames": len(self.missing_frame_ids),
            "video_gaps": self.video_gaps,
            "video": self.meta.get("video", {}),
            "errors": self.meta.get("errors", []),
            "accurate": accurate,
            "accuracy_note": accuracy_note,
            "frame_id_min": self.telemetry[0]["frame_id"] if self.telemetry else None,
            "frame_id_max": self.telemetry[-1]["frame_id"] if self.telemetry else None,
            # M3：参数修改事件（回放页签展示；旧 M2 会话无 events.jsonl 时为空列表）
            "events_available": bool(self.events),
            "events": self.events,
            "time_base": self.meta.get("time_base", ""),
            "frame_timestamp_source": self.meta.get("frame_timestamp_source", ""),
            "app_version": self.meta.get("app_version", ""),
            # M2-UI：页签 C 只读展示（配置快照文件清单 + 运行时参数），不参与时间关联
            "config_snapshot": self.config_snapshot,
        }


_SESSIONS = {}          # path -> (ReplaySession, loaded_monotonic)
_SESSIONS_LOCK = threading.Lock()
_SESSION_REFRESH_S = 2.0  # 未完成（仍在录制）的会话刷新间隔


def get_session(session_name):
    """session_name: 相对 root 的目录名，或（--dir 模式）固定会话"""
    if not session_name:
        session_name = CONFIG["single_dir"] or ""
    if CONFIG["single_dir"]:
        path = CONFIG["single_dir"]
    else:
        # 仅允许 root 下的直接子目录名，避免路径穿越
        safe = os.path.basename(session_name.strip().rstrip("/"))
        path = os.path.join(CONFIG["root"], safe) if CONFIG["root"] else ""
    if not path or not os.path.isdir(path):
        return None
    with _SESSIONS_LOCK:
        cached = _SESSIONS.get(path)
        if cached is not None:
            session, loaded_at = cached
            # 已完成会话内容不再变化，直接复用；录制中的会话按刷新间隔重新读取
            if session.complete or (time.monotonic() - loaded_at) < _SESSION_REFRESH_S:
                return session
        session = ReplaySession(path)
        _SESSIONS[path] = (session, time.monotonic())
        return session


def list_sessions():
    root = CONFIG["root"]
    if CONFIG["single_dir"]:
        session = get_session("")
        return [session.summary()] if session else []
    if not root or not os.path.isdir(root):
        return []
    out = []
    for name in sorted(os.listdir(root), reverse=True):
        path = os.path.join(root, name)
        if not os.path.isdir(path):
            continue
        if not os.path.isfile(os.path.join(path, "session.json")) and \
                not os.path.isfile(os.path.join(path, "telemetry_001.csv")):
            continue
        session = get_session(name)
        if session:
            out.append(session.summary())
    return out


# ---------------------------------------------------------------------------
# 路由
# ---------------------------------------------------------------------------
@app.route("/")
def index():
    return render_template("replay.html", live_url=CONFIG.get("live_url", ""))


@app.route("/api/sessions")
def api_sessions():
    sessions = list_sessions()
    return jsonify({"ok": True, "root": CONFIG["root"] or CONFIG["single_dir"],
                    "sessions": sessions})


@app.route("/api/session")
def api_session():
    session = get_session(request.args.get("dir", ""))
    if session is None:
        return jsonify({"ok": False, "error": "会话不存在或路径无效"}), 404
    return jsonify({
        "ok": True,
        "summary": session.summary(),
        "telemetry": session.telemetry,
        "video_index": session.video_index,
        "missing_frame_ids": session.missing_frame_ids,
        "events": [],  # M3 预留：参数修改/人工标记事件
    })


@app.route("/api/frame")
def api_frame():
    session = get_session(request.args.get("dir", ""))
    if session is None or session.video is None:
        return jsonify({"ok": False, "error": "会话或视频不存在"}), 404
    try:
        index = int(request.args.get("index", "0"))
    except ValueError:
        return jsonify({"ok": False, "error": "index 参数无效"}), 400
    if session.video_index:
        index = max(0, min(index, len(session.video_index) - 1))
    jpeg = session.video.get_jpeg(index)
    if jpeg is None:
        return jsonify({"ok": False, "error": f"无法读取视频帧 {index}"}), 404
    entry = session.video_index[index] if index < len(session.video_index) else {}
    return Response(jpeg, mimetype="image/jpeg", headers={
        "Cache-Control": "no-cache",
        "X-Video-Frame-Index": str(index),
        "X-Source-Frame-Id": str(entry.get("source_frame_id", "")),
        "X-Source-Elapsed-Ms": f"{entry.get('elapsed_ms', 0):.3f}",
    })


@app.route("/api/frame_at")
def api_frame_at():
    """按会话内相对时间返回最近的视频帧（回放播放使用）"""
    session = get_session(request.args.get("dir", ""))
    if session is None or session.video is None:
        return jsonify({"ok": False, "error": "会话或视频不存在"}), 404
    try:
        t_ms = float(request.args.get("t_ms", "0"))
    except ValueError:
        return jsonify({"ok": False, "error": "t_ms 参数无效"}), 400
    index = session.video_index_at(t_ms)
    if index is None:
        return jsonify({"ok": False, "error": "无视频帧"}), 404
    jpeg = session.video.get_jpeg(index)
    if jpeg is None:
        return jsonify({"ok": False, "error": f"无法读取视频帧 {index}"}), 404
    entry = session.video_index[index]
    return Response(jpeg, mimetype="image/jpeg", headers={
        "Cache-Control": "no-cache",
        "X-Video-Frame-Index": str(index),
        "X-Source-Frame-Id": str(entry.get("source_frame_id", "")),
        "X-Source-Elapsed-Ms": f"{entry.get('elapsed_ms', 0):.3f}",
    })


@app.route("/api/telemetry_at")
def api_telemetry_at():
    session = get_session(request.args.get("dir", ""))
    if session is None:
        return jsonify({"ok": False, "error": "会话不存在"}), 404
    try:
        t_ms = float(request.args.get("t_ms", "0"))
    except ValueError:
        return jsonify({"ok": False, "error": "t_ms 参数无效"}), 400
    row = session.telemetry_at(t_ms)
    return jsonify({"ok": True, "telemetry": row})


@app.route("/api/telemetry_by_frame")
def api_telemetry_by_frame():
    session = get_session(request.args.get("dir", ""))
    if session is None:
        return jsonify({"ok": False, "error": "会话不存在"}), 404
    try:
        frame_id = int(request.args.get("frame_id", "0"))
    except ValueError:
        return jsonify({"ok": False, "error": "frame_id 参数无效"}), 400
    row = session.frame_id_at(frame_id)
    if row is None:
        return jsonify({"ok": False, "error": f"帧 {frame_id} 不存在"}), 404
    return jsonify({"ok": True, "telemetry": row})


@app.route("/healthz")
def healthz():
    return jsonify({"ok": True, "mode": "replay"})


@app.route("/favicon.ico")
def favicon():
    return Response(status=204)


def main():
    parser = argparse.ArgumentParser(description="DART 视觉录制离线回放服务（M2）")
    parser.add_argument("--dir", default="", help="直接打开某个会话目录（可复制到任意位置）")
    parser.add_argument("--root", default="", help="会话根目录（列出其中所有会话）")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5001)
    parser.add_argument("--live-url", default="",
                        help="实时调试台地址（可选，例如 http://127.0.0.1:5000/；用于回放页面的返回链接）")
    args = parser.parse_args()

    if args.dir:
        path = os.path.abspath(args.dir)
        if not os.path.isdir(path):
            print(f"[playback] 错误: 会话目录不存在: {path}")
            return 1
        CONFIG["single_dir"] = path
    elif args.root:
        CONFIG["root"] = os.path.abspath(args.root)
        if not os.path.isdir(CONFIG["root"]):
            print(f"[playback] 错误: 会话根目录不存在: {CONFIG['root']}")
            return 1
    else:
        # 默认使用项目下 recordings/
        default_root = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.realpath(__file__)))), "recordings")
        CONFIG["root"] = default_root
        if not os.path.isdir(default_root):
            print(f"[playback] 警告: 默认会话根目录不存在: {default_root}")

    CONFIG["live_url"] = args.live_url

    print(f"[playback] 回放服务: http://{args.host}:{args.port}/", flush=True)
    print(f"[playback] 会话来源: {CONFIG['single_dir'] or CONFIG['root']}", flush=True)
    app.run(host=args.host, port=args.port, threaded=True, use_reloader=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

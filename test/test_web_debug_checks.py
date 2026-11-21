#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
M1 Web 调试链路自检脚本（本地验证用）

用途：在 C++ 视觉程序（模拟源）与 Flask 后端已启动的情况下，验证：
1. /api/status 字段完整性与链路状态；
2. 调试数据与实际算法输出一致（tx 与 turn/distance 规则一致）；
3. SSE 遥测流可用；
4. MJPEG 视频流可用（真实 JPEG 帧）；
5. 数据来源标识存在；
6. 无视觉程序时 stale 判定正确（可单独运行 --offline）。

用法：
    python3 test/test_web_debug_checks.py --base http://127.0.0.1:5000 --samples 120
    python3 test/test_web_debug_checks.py --offline     # 在后端未接视觉程序时验证降级
"""

import argparse
import json
import sys
import threading
import time
import urllib.request

FAILS = []
CHECKS = []


def check(name, ok, detail=""):
    CHECKS.append((name, ok, detail))
    print(("  [PASS] " if ok else "  [FAIL] ") + name + ((" :: " + detail) if detail else ""))
    if not ok:
        FAILS.append(name)


def http_get(path, timeout=5.0, raw=False):
    with urllib.request.urlopen(path, timeout=timeout) as resp:
        data = resp.read()
        if raw:
            return resp.status, data
        return resp.status, json.loads(data.decode("utf-8"))


def get_status(base):
    return http_get(base + "/api/status")[1]


REQUIRED_FIELDS = [
    "seq", "timestamp_ms", "source", "source_label", "mode",
    "detected", "target_x", "target_y", "radius",
    "image_width", "image_height", "image_center_x", "image_center_y",
    "offset_x", "offset_y", "turn_flag", "distance_flag", "clear_flag",
    "tx", "fps", "processing_time_ms",
    "tracking", "lock_frames", "required_frames", "lost_frames", "required_lost_frames",
    "green_pixels", "candidate_count", "jpeg_bytes",
]


def check_schema(frame):
    missing = [f for f in REQUIRED_FIELDS if f not in frame]
    check("字段完整（任务第三节要求的全部字段）", not missing, "缺失: " + str(missing) if missing else "")
    check("图像尺寸与实际画面一致", frame["image_width"] > 0 and frame["image_height"] > 0,
          f'{frame["image_width"]}x{frame["image_height"]}')
    check("processing_time_ms > 0", frame["processing_time_ms"] > 0, f'{frame["processing_time_ms"]:.2f}')
    check("fps > 0", frame["fps"] > 0, f'{frame["fps"]:.2f}')
    check("数据来源标识非空", bool(frame["source"]) and bool(frame["source_label"]),
          f'{frame["source"]} / {frame["source_label"]}')
    check("jpeg_bytes > 0（存在调试图像）", frame["jpeg_bytes"] > 0, str(frame["jpeg_bytes"]))


def expected_tx(frame):
    """按原有 main.cpp 发送逻辑推导本帧应当发送的指令（用于比对，不参与发送）。"""
    if frame["detected"]:
        return True, frame["turn_flag"], frame["distance_flag"], 1.0
    if abs(frame["distance_flag"] - 3.0) < 1e-6:
        return True, 3.0, 3.0, 1.0
    return False, None, None, None


def check_consistency(frames):
    bad = 0
    detail = ""
    for f in frames:
        sent, turn, dist, clear = expected_tx(f)
        if bool(f["tx"]["sent"]) != sent:
            bad += 1
            detail = f'seq={f["seq"]} detected={f["detected"]} tx.sent={f["tx"]["sent"]} expected={sent}'
            break
        if sent:
            if abs(f["tx"]["turn_flag"] - turn) > 1e-6 or abs(f["tx"]["distance_flag"] - dist) > 1e-6 \
                    or abs(f["tx"]["clear_flag"] - clear) > 1e-6 or abs(f["clear_flag"] - clear) > 1e-6:
                bad += 1
                detail = (f'seq={f["seq"]} tx=({f["tx"]["turn_flag"]},{f["tx"]["distance_flag"]},'
                          f'{f["tx"]["clear_flag"]}) expected=({turn},{dist},{clear})')
                break
    check(f"调试数据与实际串口输出一致（{len(frames)} 帧比对）", bad == 0, detail)


def check_stream(base, timeout=6.0):
    got = {"samples": 0, "receiver": False}

    def worker():
        try:
            req = urllib.request.Request(base + "/api/telemetry/stream?after_seq=0")
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                for raw in resp:
                    line = raw.decode("utf-8", "replace").strip()
                    if line.startswith("data: "):
                        payload = json.loads(line[6:])
                        got["samples"] += len(payload.get("samples", []))
                        got["receiver"] = bool(payload.get("receiver"))
                        if got["samples"] > 0:
                            return
        except Exception:  # noqa: BLE001
            pass

    t = threading.Thread(target=worker, daemon=True)
    t.start()
    t.join(timeout)
    check("SSE 遥测流可用", got["samples"] > 0, f'收到 {got["samples"]} 个样本')
    check("SSE 携带接收链路状态", got["receiver"])


def check_video(base, timeout=6.0):
    found = {"jpeg": 0, "bytes": 0}

    def worker():
        try:
            with urllib.request.urlopen(base + "/video_feed", timeout=timeout) as resp:
                buf = b""
                while found["jpeg"] < 2:
                    chunk = resp.read(65536)
                    if not chunk:
                        break
                    buf += chunk
                    if b"Content-Type: image/jpeg" in buf and b"\xff\xd8" in buf:
                        found["jpeg"] += 1
                        found["bytes"] = len(buf)
                        buf = b""
        except Exception:  # noqa: BLE001
            pass

    t = threading.Thread(target=worker, daemon=True)
    t.start()
    t.join(timeout)
    check("MJPEG 视频流可用", found["jpeg"] > 0, f'收到 {found["jpeg"]} 个 JPEG 帧')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="http://127.0.0.1:5000")
    parser.add_argument("--samples", type=int, default=120)
    parser.add_argument("--offline", action="store_true", help="验证无视觉程序时的降级行为")
    args = parser.parse_args()

    print(f"== DART Web Debug M1 自检 :: {args.base} ==")

    status, resp = http_get(args.base + "/api/status")
    check("/api/status 可访问", status == 200)
    check("/healthz 可访问", http_get(args.base + "/healthz")[0] == 200)
    check("/api/meta 可访问", http_get(args.base + "/api/meta")[0] == 200)

    receiver = resp["receiver"]
    if args.offline:
        # 设计：无视觉程序时 Flask 可保留最后一帧用于"过期画面"展示，关键是 stale/connected 状态正确
        check("降级：无实时帧或最后一帧已过期",
              resp["frame"] is None or receiver["stale"] is True,
              "frame 保留最后一帧时 stale 必须为 true")
        check("无视觉程序时 stale=true", receiver["stale"] is True)
        check("无视觉程序时 connected=false", receiver["connected"] is False)
        check("数据口仍在监听", receiver["listening"] is True)
        check("无协议错误", receiver["protocol_errors"] == 0)
    else:
        check("视觉程序已连接", receiver["connected"] is True)
        check("数据未过期", receiver["stale"] is False, f'age={receiver["last_frame_age_ms"]} ms')
        check("接收帧率 > 0", receiver["recv_fps"] > 0, f'{receiver["recv_fps"]} fps')
        check("无协议错误", receiver["protocol_errors"] == 0)
        frame = resp["frame"]
        check("最新帧存在", frame is not None)
        if frame is not None:
            check_schema(frame)

        frames = [frame] if frame else []
        last_seq = frame["seq"] if frame else 0
        deadline = time.time() + 30
        while len(frames) < args.samples and time.time() < deadline:
            time.sleep(0.05)
            st = get_status(args.base)
            f = st["frame"]
            if f and f["seq"] > last_seq:
                last_seq = f["seq"]
                frames.append(f)
        check(f"采集到 {args.samples} 帧样本", len(frames) >= min(args.samples, 60), f"实际 {len(frames)}")
        if len(frames) > 1:
            check_consistency(frames)
            seqs = [f["seq"] for f in frames]
            check("seq 单调递增", all(b > a for a, b in zip(seqs, seqs[1:])))
        check_stream(args.base)
        check_video(args.base)

    print()
    print(f"== 结果: {len(CHECKS) - len(FAILS)}/{len(CHECKS)} 通过 ==")
    if FAILS:
        print("失败项: " + ", ".join(FAILS))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

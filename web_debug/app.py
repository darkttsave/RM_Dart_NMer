#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
M1 Web 调试后端（Flask）

职责：
1. 在独立端口接收 C++ 视觉程序通过 TCP 发来的调试帧（JSON 遥测 + JPEG 图像）；
2. 以 MJPEG 向浏览器推送实时视觉画面；
3. 以 SSE（Server-Sent Events）向浏览器推送实时遥测样本（用于曲线与状态面板）；
4. 提供 REST 快照接口，便于自检与后续 M2 扩展。

设计约束（对应任务第五节）：
- 本进程与视觉主链路完全解耦：Flask 未启动 / 浏览器未连接 / 浏览器断开，
  都只会让 C++ 侧丢弃调试帧，不会阻塞视觉线程，也不会产生任何控制指令。
- 本文件只做"接收 -> 缓存 -> 转发"，不修改原始数据，不向下位机发送任何内容。

帧格式（与 web_debug/DebugTelemetry.cpp 一致，小端）：
    magic[4]="DWDB" | version u8 | flags u8 | reserved u16
    | json_len u32 | jpeg_len u32 | json bytes | jpeg bytes
"""

import argparse
import json
import socket
import struct
import threading
import time
from collections import deque

from flask import Flask, Response, jsonify, render_template, request, stream_with_context

MAGIC = b"DWDB"
HEADER = struct.Struct("<4sBBHII")  # 16 bytes
MAX_JSON_BYTES = 256 * 1024
MAX_JPEG_BYTES = 8 * 1024 * 1024
STALE_AFTER_S = 1.5          # 超过该时间无新帧，前端视为"数据过期"
DEFAULT_CONTROL_PORT = 5011  # C++ 录制控制口（仅本机回环），/api/recorder 转发到这里
HISTORY_MAXLEN = 1800        # SSE 曲线样本缓存（约 20fps * 90s）

app = Flask(__name__, static_folder="static", template_folder="templates")
# 本地调试工具：模板改动后无需重启进程即可生效
app.config["TEMPLATES_AUTO_RELOAD"] = True
app.jinja_env.auto_reload = True


# --------------------------------------------------------------------------
# C++ 调试帧接收器
# --------------------------------------------------------------------------
class FrameReceiver(threading.Thread):
    def __init__(self, host, port, name="debug-frame-receiver"):
        super().__init__(name=name, daemon=True)
        self.host = host
        self.port = port
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)

        self._latest_json = None          # dict（不含图像）
        self._latest_jpeg = b""
        self._latest_seq = 0
        self._latest_recv_mono = 0.0
        self._history = deque(maxlen=HISTORY_MAXLEN)  # [(seq, dict), ...]

        self._connected = False
        self._client_addr = ""
        self._total_frames = 0
        self._protocol_errors = 0
        self._overwritten = 0
        self._listen_error = ""
        self._recv_times = deque(maxlen=120)  # 用于计算接收帧率
        self._stop = threading.Event()

    # -------- 生命周期 --------
    def run(self):
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            server.bind((self.host, self.port))
            server.listen(2)
        except OSError as error:
            self._listen_error = f"{self.host}:{self.port} 监听失败: {error}"
            print(f"[backend] ERROR {self._listen_error}", flush=True)
            return

        server.settimeout(0.5)
        print(f"[backend] 等待视觉程序连接: tcp://{self.host}:{self.port}", flush=True)
        while not self._stop.is_set():
            try:
                conn, addr = server.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            print(f"[backend] 视觉程序已连接: {addr[0]}:{addr[1]}", flush=True)
            with self._cond:
                self._connected = True
                self._client_addr = f"{addr[0]}:{addr[1]}"
            try:
                self._serve_connection(conn)
            finally:
                try:
                    conn.close()
                except OSError:
                    pass
                with self._cond:
                    self._connected = False
                print("[backend] 视觉程序连接已断开", flush=True)
        try:
            server.close()
        except OSError:
            pass

    def stop(self):
        self._stop.set()

    # -------- 连接处理 --------
    def _serve_connection(self, conn):
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        buf = bytearray()
        while not self._stop.is_set():
            try:
                chunk = conn.recv(65536)
            except (socket.timeout, OSError):
                break
            if not chunk:
                break
            buf.extend(chunk)
            while True:
                if len(buf) < HEADER.size:
                    break
                magic, version, _flags, _reserved, json_len, jpeg_len = HEADER.unpack_from(buf, 0)
                if magic != MAGIC:
                    self._protocol_errors += 1
                    buf.clear()
                    break
                if (version != 1 or json_len > MAX_JSON_BYTES or jpeg_len > MAX_JPEG_BYTES):
                    self._protocol_errors += 1
                    buf.clear()
                    break
                total = HEADER.size + json_len + jpeg_len
                if len(buf) < total:
                    break
                json_bytes = bytes(buf[HEADER.size:HEADER.size + json_len])
                jpeg_bytes = bytes(buf[HEADER.size + json_len:total])
                del buf[:total]
                self._ingest(json_bytes, jpeg_bytes)

    def _ingest(self, json_bytes, jpeg_bytes):
        try:
            payload = json.loads(json_bytes.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self._protocol_errors += 1
            return

        now = time.monotonic()
        seq = int(payload.get("seq", 0))
        with self._cond:
            self._latest_json = payload
            self._latest_jpeg = jpeg_bytes
            self._latest_seq = seq
            self._latest_recv_mono = now
            self._total_frames += 1
            self._recv_times.append(now)
            # 曲线样本（去掉大字段？JSON 本身很小，直接存）
            self._history.append((seq, payload))
            self._cond.notify_all()

    # -------- 对外读取接口 --------
    def wait_latest(self, last_seq, timeout=1.0):
        """等待比 last_seq 更新的帧；返回 (seq, jpeg) 或 None。"""
        deadline = time.monotonic() + timeout
        with self._cond:
            while self._latest_seq <= last_seq or self._latest_seq == 0:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or self._stop.is_set():
                    return None
                self._cond.wait(remaining)
            if not self._latest_jpeg:
                return None
            return self._latest_seq, self._latest_jpeg

    def samples_after(self, after_seq):
        with self._lock:
            return [dict(seq=s, payload=p) for s, p in self._history if s > after_seq]

    def status(self):
        with self._lock:
            now = time.monotonic()
            age_ms = None
            if self._latest_recv_mono > 0:
                age_ms = round((now - self._latest_recv_mono) * 1000.0, 1)
            recv_fps = 0.0
            if len(self._recv_times) >= 2:
                span = self._recv_times[-1] - self._recv_times[0]
                if span > 0:
                    recv_fps = round((len(self._recv_times) - 1) / span, 2)
            return {
                "listening": self._listen_error == "",
                "listen_error": self._listen_error,
                "connected": self._connected,
                "client": self._client_addr,
                "total_frames": self._total_frames,
                "recv_fps": recv_fps,
                "last_seq": self._latest_seq,
                "last_frame_age_ms": age_ms,
                "stale": age_ms is None or age_ms > STALE_AFTER_S * 1000.0,
                "protocol_errors": self._protocol_errors,
            }

    def snapshot(self):
        # 注意：status() 内部会再次获取 self._lock，这里必须在锁外调用，避免死锁
        with self._lock:
            payload = dict(self._latest_json) if self._latest_json else None
        return payload, self.status()


# --------------------------------------------------------------------------
# 录制控制（转发到 C++ 本机控制口；Flask 只是控制入口，录制执行主体在 C++ 侧）
# --------------------------------------------------------------------------
def recorder_command(cmd, control_port=DEFAULT_CONTROL_PORT):
    """向 C++ 本机控制口（127.0.0.1）发送命令并返回 JSON 响应。

    cmd 既可以是字符串（status/start/stop/params/params.save），
    也可以是带 payload 的字典（如 {"cmd": "params.apply", "values": {...}}）。
    """
    if not control_port:
        return {"ok": False, "error": "录制控制口未配置"}
    payload = {"cmd": cmd} if isinstance(cmd, str) else dict(cmd)
    try:
        with socket.create_connection(("127.0.0.1", int(control_port)), timeout=1.5) as sock:
            sock.sendall(json.dumps(payload).encode("utf-8") + b"\n")
            sock.settimeout(5.0)
            data = sock.recv(65536)
        if not data:
            return {"ok": False, "error": "录制控制口无响应"}
        return json.loads(data.decode("utf-8").strip())
    except (OSError, ValueError) as error:
        return {"ok": False, "error": f"录制控制口不可用: {error}"}


# --------------------------------------------------------------------------
# Flask 路由
# --------------------------------------------------------------------------
receiver: FrameReceiver = None  # 在 main 中创建


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/video_feed")
def video_feed():
    """MJPEG 实时视觉画面。浏览器断开时生成器自然结束，不影响接收线程。"""

    def generate():
        last_seq = 0
        while True:
            frame = receiver.wait_latest(last_seq, timeout=1.0)
            if frame is None:
                continue
            last_seq, jpeg = frame
            yield (b"--frame\r\n"
                   b"Content-Type: image/jpeg\r\n"
                   b"Content-Length: " + str(len(jpeg)).encode("ascii") + b"\r\n\r\n" +
                   jpeg + b"\r\n")

    return Response(generate(), mimetype="multipart/x-mixed-replace; boundary=frame",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@app.route("/api/status")
def api_status():
    """一次性快照：最新调试帧 + 接收链路状态（页面初始化与自检用）。"""
    payload, status = receiver.snapshot()
    return jsonify({
        "ok": True,
        "frame": payload,
        "receiver": status,
        "service": _service_info(),
    })


@app.route("/api/telemetry/stream")
def api_telemetry_stream():
    """SSE 遥测样本流（含接收链路状态），用于状态面板与实时曲线。"""
    try:
        start_seq = int(request.args.get("after_seq", "0"))
    except ValueError:
        start_seq = 0

    @stream_with_context
    def generate():
        last_seq = start_seq
        while True:
            samples = receiver.samples_after(last_seq)
            status = receiver.status()
            if samples:
                last_seq = int(samples[-1]["seq"])
                payload = {"samples": samples, "next_seq": last_seq,
                           "receiver": status, "service": _service_info()}
                yield "data: " + json.dumps(payload, ensure_ascii=False,
                                            separators=(",", ":")) + "\n\n"
            else:
                # 心跳：保持连接，同时让前端及时更新链路状态
                yield ": keepalive " + json.dumps(status, ensure_ascii=False) + "\n\n"
            time.sleep(0.1)

    return Response(generate(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no",
                             "Connection": "keep-alive"})


@app.route("/api/recorder", methods=["GET", "POST"])
def api_recorder():
    """录制状态查询（GET）与开始/停止（POST）。

    POST body: {"cmd": "start"|"stop"|"status"} 或 {"enabled": true|false}
    实际录制由 C++ 主程序执行，本接口仅转发到其本机控制口（127.0.0.1）。
    """
    control_port = app.config.get("RECORDER_CONTROL_PORT", DEFAULT_CONTROL_PORT)
    if request.method == "POST":
        payload = request.get_json(silent=True) or {}
        cmd = payload.get("cmd")
        if not cmd and "enabled" in payload:
            cmd = "start" if payload.get("enabled") else "stop"
        if cmd not in ("start", "stop", "status"):
            return jsonify({"ok": False, "error": "cmd 必须是 start/stop/status"}), 400
        return jsonify(recorder_command(cmd, control_port))
    return jsonify(recorder_command("status", control_port))


@app.route("/api/params", methods=["GET", "POST"])
def api_params():
    """运行时参数：查询（GET）与应用/保存（POST）。

    GET  -> 转发控制口 params，返回 running / saved / dirty / params 元数据；
    POST -> {"cmd":"apply","values":{...}} 应用运行参数，或 {"cmd":"save"} 写入 YAML。
    校验与生效以 C++ 返回为准（C++ 运行时状态是实际生效值的权威来源）。
    """
    control_port = app.config.get("RECORDER_CONTROL_PORT", DEFAULT_CONTROL_PORT)
    if request.method == "GET":
        return jsonify(recorder_command("params", control_port))
    payload = request.get_json(silent=True) or {}
    cmd = payload.get("cmd")
    if cmd == "apply":
        values = payload.get("values")
        if not isinstance(values, dict) or not values:
            return jsonify({"ok": False, "error": "values 必须是非空对象"}), 400
        return jsonify(recorder_command({"cmd": "params.apply", "values": values}, control_port))
    if cmd == "save":
        return jsonify(recorder_command("params.save", control_port))
    return jsonify({"ok": False, "error": "cmd 必须是 apply / save"}), 400


@app.route("/api/meta")
def api_meta():
    """接口与字段说明（含 M2 预留接口位置）。"""
    return jsonify(_service_info())


@app.route("/healthz")
def healthz():
    return jsonify({"ok": True})


@app.route("/favicon.ico")
def favicon():
    """避免浏览器请求 favicon 产生 404 日志。"""
    return Response(status=204)


def _service_info():
    return {
        "name": "DART Web Debug",
        "stage": "M1",
        "protocol_version": 1,
        "stale_after_ms": int(STALE_AFTER_S * 1000),
        "endpoints": {
            "video_feed": "/video_feed (MJPEG)",
            "status": "/api/status",
            "telemetry_stream": "/api/telemetry/stream (SSE)",
            "meta": "/api/meta",
        },
        # M2 已实现：录制控制（转发到 C++ 本机控制口）；其余仍预留
        "m2": {
            "recorder": "/api/recorder (GET 状态 / POST start|stop)",
            "playback": "独立回放服务 web_debug/playback.py（--dir/--root）",
        },
        "m3": {
            "params": "/api/params (GET 运行参数/配置值 / POST apply|save)",
            "events": "录制会话 events.jsonl（参数修改事件，回放页签展示）",
        },
        # M3+ 预留：电控遥测、参数管理
        "reserved": {
            "board_telemetry": "/api/board/telemetry (预留)",
            "parameters": "/api/config (预留)",
        },
    }


# --------------------------------------------------------------------------
# 入口
# --------------------------------------------------------------------------
def create_receiver(host, port):
    global receiver
    receiver = FrameReceiver(host, port)
    receiver.start()


def main():
    parser = argparse.ArgumentParser(description="DART 视觉 Web 调试后端（M1）")
    parser.add_argument("--host", default="127.0.0.1", help="浏览器访问监听地址（默认 127.0.0.1）")
    parser.add_argument("--port", type=int, default=5000, help="浏览器访问端口（默认 5000）")
    parser.add_argument("--data-host", default="127.0.0.1", help="接收 C++ 调试帧的监听地址")
    parser.add_argument("--data-port", type=int, default=5010, help="接收 C++ 调试帧的端口（默认 5010）")
    parser.add_argument("--control-port", type=int, default=DEFAULT_CONTROL_PORT,
                        help="C++ 录制控制口（默认 5011；0=禁用 /api/recorder）")
    args = parser.parse_args()

    app.config["RECORDER_CONTROL_PORT"] = args.control_port
    create_receiver(args.data_host, args.data_port)
    print(f"[backend] Web 调试界面: http://{args.host}:{args.port}/", flush=True)
    app.run(host=args.host, port=args.port, threaded=True, use_reloader=False)


if __name__ == "__main__":
    main()

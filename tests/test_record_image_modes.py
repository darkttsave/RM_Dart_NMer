#!/usr/bin/env python3
"""端到端检查录像来源、会话锁定及回放；使用临时配置和合成静态图。"""
import csv
import json
import os
from pathlib import Path
import re
import shutil
import signal
import statistics
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(url, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=8) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


def wait_for(predicate, timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            value = predicate()
            if value:
                return value
        except (OSError, ValueError, KeyError):
            pass
        time.sleep(.1)
    raise AssertionError("等待服务或录像超时")


def stop_process(process):
    if process.poll() is None:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    with tempfile.TemporaryDirectory(prefix="dart-record-modes-") as tmp:
        base = Path(tmp)
        (base / "config").mkdir()
        (base / "build").mkdir()
        control_port, web_port, data_port = free_port(), free_port(), free_port()
        config = (ROOT / "config/debug.yaml").read_text()
        config = re.sub(r"(?m)^(\s*control_port:)\s*\d+", rf"\g<1> {control_port}", config)
        config = re.sub(r"(?m)^(  port:)\s*5010\b", rf"\g<1> {data_port}", config)
        (base / "config/debug.yaml").write_text(config)
        image = np.zeros((480, 640, 3), np.uint8)
        cv2.circle(image, (220, 240), 20, (0, 255, 0), -1)
        cv2.imwrite(str(base / "source.png"), image)
        output = base / "recordings"
        log = open(base / "process.log", "w")
        auto_root = base / "auto-recordings"
        auto = subprocess.Popen([str(ROOT / "build/main"), "--source", f"image:{base / 'source.png'}",
                                 "--no-serial", "--no-gui", "--no-web-debug", "--auto-record",
                                 "--record-root", str(auto_root)], cwd=base / "build", stdout=log, stderr=log)
        try:
            wait_for(lambda: list(auto_root.glob("*/debug_001.avi")))
            time.sleep(.5)
        finally:
            stop_process(auto)
        auto_session = next(auto_root.iterdir())
        assert json.loads((auto_session / "session.json").read_text())["record_image_mode"] == "debug"
        assert cv2.VideoCapture(str(auto_session / "debug_001.avi")).isOpened()
        print("PASS 自动录制默认 debug，退出完成会话")
        cpp = subprocess.Popen([str(ROOT / "build/main"), "--source", f"image:{base / 'source.png'}",
                                "--no-serial", "--no-gui", "--no-auto-record",
                                "--record-root", str(output)], cwd=base / "build", stdout=log, stderr=log)
        flask = None
        try:
            # 先通过 C++ 控制口启动，验证未指定模式默认 debug 且不需要 Flask。
            def control(payload):
                with socket.create_connection(("127.0.0.1", control_port), timeout=2) as sock:
                    sock.sendall(json.dumps(payload).encode() + b"\n")
                    return json.loads(sock.recv(65536))
            wait_for(lambda: control({"cmd": "status"})["ok"])
            status = control({"cmd": "start"})
            assert status["ok"] and status["record_image_mode"] == "debug", status
            wait_for(lambda: control({"cmd": "status"})["video_frames"] >= 4)
            control({"cmd": "stop"})
            default_dir = Path(status["session_dir"])
            assert (default_dir / "debug_001.avi").exists()
            raw_without_flask = control({"cmd": "start", "record_image_mode": "raw"})
            assert raw_without_flask["ok"] and raw_without_flask["record_image_mode"] == "raw"
            wait_for(lambda: control({"cmd": "status"})["video_frames"] >= 4)
            control({"cmd": "stop"})
            assert (Path(raw_without_flask["session_dir"]) / "raw_001.avi").exists()
            print("PASS T01/T09: 默认 debug；无 Flask 时 debug 和 raw 录制")

            flask = subprocess.Popen([sys.executable, str(ROOT / "debug/web/app.py"), "--port", str(web_port),
                                      "--data-port", str(data_port), "--control-port", str(control_port)],
                                     cwd=ROOT, stdout=log, stderr=log)
            api = f"http://127.0.0.1:{web_port}/api/recorder"
            wait_for(lambda: request(api)[1].get("ok"))
            assert request(api, {"cmd": "start", "record_image_mode": "bad"})[0] == 400
            for invalid in (None, 4, "", [], {}):
                assert request(api, {"cmd": "start", "record_image_mode": invalid})[0] == 400
                assert not control({"cmd": "start", "record_image_mode": invalid})["ok"]
            recordings = {}
            for mode in ("raw", "debug"):
                _, status = request(api, {"cmd": "start", "record_image_mode": mode})
                assert status["ok"] and status["record_image_mode"] == mode, status
                session = Path(status["session_dir"])
                wait_for(lambda: request(api)[1]["video_frames"] >= 15)
                time.sleep(2)  # 等待稳定采样，而非仅比较启动阶段
                _, rejected = request(api, {"cmd": "start", "record_image_mode": "debug" if mode == "raw" else "raw"})
                assert not rejected["ok"]
                assert request(api)[1]["record_image_mode"] == mode
                if mode == "raw" and os.environ.get("DART_TEST_M3") == "1":
                    subprocess.run([sys.executable, str(ROOT / "tests/test_m3_params.py"),
                                    "--base", f"http://127.0.0.1:{web_port}",
                                    "--config", str(base / "config/debug.yaml"),
                                    "--session", str(session)], check=True)
                _, stopped = request(api, {"cmd": "stop"})
                assert stopped["ok"] and not stopped["recording"]
                meta = json.loads((session / "session.json").read_text())
                assert meta["record_image_mode"] == mode and meta["video"]["file"] == f"{mode}_001.avi"
                assert (session / f"{mode}_001.avi").exists()
                assert not (session / f"{'raw' if mode == 'debug' else 'debug'}_001.avi").exists()
                with open(session / "telemetry_001.csv") as f:
                    rows = list(csv.DictReader(f))
                with open(session / "video_index_001.csv") as f:
                    indices = list(csv.DictReader(f))
                ids = {row["frame_id"]: row for row in rows}
                assert indices and all(row["source_frame_id"] in ids and
                    int(row["source_timestamp_us"]) == int(ids[row["source_frame_id"]]["timestamp_us"]) and
                    int(row["source_elapsed_us"]) == int(ids[row["source_frame_id"]]["elapsed_us"]) for row in indices)
                assert all(int(a["source_elapsed_us"]) <= int(b["source_elapsed_us"]) for a, b in zip(indices, indices[1:]))
                assert any(row["detected"] == "1" for row in rows), "fixture must detect green target"
                cap = cv2.VideoCapture(str(session / f"{mode}_001.avi"))
                frames = []
                while True:
                    ok, frame = cap.read()
                    if not ok: break
                    frames.append(frame)
                cap.release()
                assert len(frames) == len(indices)
                # 固定源图：JPEG 压缩允许小幅偏差；所有帧都必须符合该会话的来源。
                differences = [int(np.count_nonzero(np.max(cv2.absdiff(frame, image), axis=2) > 45)) for frame in frames]
                if mode == "raw": assert max(differences) < 1200, (mode, min(differences), max(differences))
                else: assert min(differences) > 1200, (mode, min(differences), max(differences))
                recordings[mode] = session
                fps = statistics.median(float(row["fps"]) for row in rows if float(row["fps"]) > 0)
                print(f"PASS {mode}: frames={len(frames)}, changed_pixels={min(differences)}..{max(differences)}, "
                      f"dropped={meta['counts']['video_frames_dropped']}, median_fps={fps:.1f}")
            print("PASS T02-T05/T08: 来源、固定模式、重启切换、索引与遥测")

            sys.path.insert(0, str(ROOT / "debug/web"))
            import playback
            playback.CONFIG["root"] = str(output)
            playback.CONFIG["single_dir"] = ""
            # 构造无 mode 字段的旧格式会话，另测试现有历史数据（存在时）。
            legacy = output / "legacy"
            shutil.copytree(default_dir, legacy)
            meta_path = legacy / "session.json"
            meta = json.loads(meta_path.read_text()); meta.pop("record_image_mode", None)
            meta_path.write_text(json.dumps(meta))
            with playback.app.test_client() as client:
                for name in ["legacy", recordings["raw"].name, recordings["debug"].name]:
                    for path in (f"/api/session?dir={name}", f"/api/frame?dir={name}&index=0", f"/api/frame_at?dir={name}&t_ms=0"):
                        assert client.get(path).status_code == 200, path
                historical = ROOT / "recordings/2026-11-21_10-38-55"
                if historical.is_dir():
                    playback.CONFIG["root"] = str(historical.parent)
                    assert client.get(f"/api/frame?dir={historical.name}&index=0").status_code == 200
                    print("PASS historical pre-existing session")
            print("PASS T06/T07: 旧格式及两种新格式回放")
            if os.environ.get("DART_TEST_UI") == "1":
                subprocess.run(["node", str(ROOT / "tests/test_record_mode_ui.js"),
                                f"http://127.0.0.1:{web_port}/"], check=True, env=os.environ)
        finally:
            if flask: stop_process(flask)
            stop_process(cpp)
            log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

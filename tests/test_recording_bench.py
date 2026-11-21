#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
M2 录制性能对照测试（测试 B）

三种配置各运行指定秒数：
  1) baseline：不录制、不预览（--no-record --no-web-debug）
  2) record_only：仅本地录制（--no-web-debug）
  3) record_preview：录制 + M1 实时预览（默认）

采集：算法帧率（主循环统计）、视频写入帧数/实际 fps、遥测行数、丢帧、
      CPU（单核百分比）、RSS，以及会话目录与 session.json 完整性。

用法：
    python3 tests/test_recording_bench.py [--seconds 20] [--skip-baseline]
"""

import argparse
import glob
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
LOG_DIR = os.path.join(ROOT, "logs", "m2_test")
RECORD_ROOT = os.path.join(ROOT, "recordings")


def read_proc(pid):
    """返回 (累计 CPU 秒, RSS MB)；进程不存在返回 None"""
    try:
        with open(f"/proc/{pid}/stat", "r") as handle:
            fields = handle.read().split()
        utime, stime = int(fields[13]), int(fields[14])
        clk = os.sysconf("SC_CLK_TCK")
        cpu_s = (utime + stime) / clk
        rss_mb = 0.0
        with open(f"/proc/{pid}/status", "r") as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    rss_mb = int(line.split()[1]) / 1024.0
                    break
        return cpu_s, rss_mb
    except (FileNotFoundError, ProcessLookupError):
        return None


def parse_log(log_path):
    loop_fps = []
    published = []
    recorder_stop = None
    with open(log_path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            m = re.search(r"\[Main\] loop fps=([\d.]+) frames=(\d+) frame_ms=([\d.]+)", line)
            if m:
                loop_fps.append(float(m.group(1)))
            m = re.search(r"\[WebDebug\] published=(\d+) sent=(\d+) dropped=(\d+) connected=(\w+)", line)
            if m:
                published.append({"published": int(m.group(1)), "sent": int(m.group(2)),
                                  "dropped": int(m.group(3)), "connected": m.group(4)})
            if "[Recorder] 录制停止" in line:
                recorder_stop = line.strip()
    return loop_fps, published, recorder_stop


def run_config(name, extra_args, seconds):
    before = set(glob.glob(os.path.join(RECORD_ROOT, "*")))
    log_path = os.path.join(LOG_DIR, f"bench_{name}.log")
    os.makedirs(LOG_DIR, exist_ok=True)
    with open(log_path, "w", encoding="utf-8") as log:
        proc = subprocess.Popen(
            [os.path.join(BUILD, "main"), "--source", "synthetic", "--no-serial", "--no-gui"] + extra_args,
            cwd=BUILD, stdout=log, stderr=subprocess.STDOUT,
        )

    cpu_samples = []
    rss_samples = []
    wall_start = time.time()
    while time.time() - wall_start < seconds:
        time.sleep(0.5)
        sample = read_proc(proc.pid)
        if sample is None:
            break
        cpu_s, rss_mb = sample
        cpu_samples.append((time.time() - wall_start, cpu_s))
        rss_samples.append(rss_mb)

    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
    try:
        exit_code = proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()
        exit_code = -9
    wall_s = time.time() - wall_start

    # CPU 平均（单核百分比）
    cpu_percent = 0.0
    if len(cpu_samples) >= 2:
        t0, c0 = cpu_samples[0]
        t1, c1 = cpu_samples[-1]
        if t1 - t0 > 0.1:
            cpu_percent = (c1 - c0) / (t1 - t0) * 100.0
    rss_peak = max(rss_samples) if rss_samples else 0.0

    loop_fps, published, recorder_stop = parse_log(log_path)

    # 本次新产生的录制会话
    after = set(glob.glob(os.path.join(RECORD_ROOT, "*")))
    new_sessions = sorted(after - before)
    session_info = None
    session_dir = new_sessions[-1] if new_sessions else None
    if session_dir:
        session_json = os.path.join(session_dir, "session.json")
        if os.path.isfile(session_json):
            with open(session_json, "r", encoding="utf-8") as handle:
                meta = json.load(handle)
            duration = float(meta.get("duration_s") or 0.0)
            counts = meta.get("counts", {})
            frames = int(counts.get("frames_processed", 0))
            video = int(counts.get("video_frames_written", 0))
            telemetry = int(counts.get("telemetry_rows", 0))
            session_info = {
                "dir": session_dir,
                "status": meta.get("status"),
                "duration_s": round(duration, 3),
                "frames_processed": frames,
                "video_frames_written": video,
                "telemetry_rows": telemetry,
                "video_frames_dropped": int(counts.get("video_frames_dropped", 0)),
                "telemetry_rows_dropped": int(counts.get("telemetry_rows_dropped", 0)),
                "write_errors": int(counts.get("write_errors", 0)),
                "algo_fps": round(frames / duration, 2) if duration > 0 else None,
                "video_fps": round(video / duration, 2) if duration > 0 else None,
                "telemetry_fps": round(telemetry / duration, 2) if duration > 0 else None,
                "video_coverage_pct": round(100.0 * video / frames, 2) if frames else None,
            }

    return {
        "name": name,
        "args": extra_args,
        "wall_s": round(wall_s, 2),
        "exit_code": exit_code,
        "loop_fps_avg": round(sum(loop_fps) / len(loop_fps), 2) if loop_fps else None,
        "loop_fps_samples": loop_fps,
        "cpu_percent_avg": round(cpu_percent, 1),
        "rss_mb_peak": round(rss_peak, 1),
        "webdebug_last": published[-1] if published else None,
        "recorder_stop_log": recorder_stop,
        "session": session_info,
        "log": log_path,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=20.0, help="每种配置运行秒数")
    parser.add_argument("--out", default=os.path.join(LOG_DIR, "bench_result.json"))
    parser.add_argument("--configs", default="", help="只运行指定配置（逗号分隔）")
    parser.add_argument("--flask", action="store_true", help="测试前启动 Flask 实时后端（真实预览）")
    args = parser.parse_args()

    configs = [
        ("baseline_no_record_no_preview", ["--no-record", "--no-web-debug"]),
        ("record_only", ["--no-web-debug"]),
        ("record_with_preview", []),
    ]
    if args.configs:
        wanted = {name.strip() for name in args.configs.split(",") if name.strip()}
        configs = [cfg for cfg in configs if cfg[0] in wanted]

    flask_proc = None
    if args.flask:
        flask_log = open(os.path.join(LOG_DIR, "bench_flask.log"), "w", encoding="utf-8")
        flask_proc = subprocess.Popen(
            [sys.executable, os.path.join(ROOT, "debug", "web", "app.py"),
             "--host", "127.0.0.1", "--port", "5000",
             "--data-host", "127.0.0.1", "--data-port", "5010"],
            cwd=ROOT, stdout=flask_log, stderr=subprocess.STDOUT,
        )
        time.sleep(2.0)
        print("[bench] Flask 实时后端已启动（真实预览）", flush=True)

    results = []
    try:
        for name, extra in configs:
            print(f"== 运行 {name}（{args.seconds:.0f}s）==", flush=True)
            result = run_config(name, extra, args.seconds)
            results.append(result)
            print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    finally:
        if flask_proc is not None:
            flask_proc.send_signal(signal.SIGTERM)
            try:
                flask_proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                flask_proc.kill()

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump({"seconds": args.seconds, "results": results}, handle, ensure_ascii=False, indent=2)
    print(f"\n结果已保存: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/bin/bash
# 看门狗：无限循环运行主程序，退出后等待3秒重启

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

while true; do
    bash "$SCRIPT_DIR/build_and_run.sh"
    echo "[Watchdog] 程序退出，1秒后重启..."
    sleep 1
done

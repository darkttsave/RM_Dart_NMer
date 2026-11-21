#!/usr/bin/env bash
# =============================================================================
# DART 视觉 Web 调试 · M1 启动脚本
#
# 用途：启动 Flask 调试后端 + C++ 视觉主程序（可选模拟视频源，无需工业相机）。
#
# 用法：
#   ./run_web_debug.sh                        # 工业相机 + 真实串口 + 实时预览（录制需网页手动开始）
#   ./run_web_debug.sh --auto-record          # 实时预览 + 启动即开始录制
#   ./run_web_debug.sh --record-only          # 无预览录制：不启动预览界面，按配置参数自动录制
#   ./run_web_debug.sh --source synthetic     # 无相机：合成绿色引导灯视频源
#   ./run_web_debug.sh --source image:tests/test.png --no-serial --no-gui
#   ./run_web_debug.sh --no-record            # 关闭录制
#   ./run_web_debug.sh --backend-only         # 只启动 Flask（视觉程序另行启动）
#   ./run_web_debug.sh --replay               # 只启动 M2 离线回放（默认 recordings/）
#   ./run_web_debug.sh --replay --session <会话目录>
#   ./run_web_debug.sh --replay --session-root <会话根目录>
#
# 说明：
#   - Flask 与视觉程序是两个独立进程，调试系统故障不会影响视觉/串口主链路；
#   - 浏览器地址默认 http://127.0.0.1:5000/ ；
#   - 调试数据口（C++ -> Flask）默认 127.0.0.1:5010。
# =============================================================================
set -u

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"
LOG_DIR="$ROOT_DIR/logs/web_debug_m1"
mkdir -p "$LOG_DIR"

WEB_HOST="${WEB_HOST:-127.0.0.1}"
WEB_PORT="${WEB_PORT:-5000}"
DATA_HOST="${DATA_HOST:-127.0.0.1}"
DATA_PORT="${DATA_PORT:-5010}"
REPLAY_PORT="${REPLAY_PORT:-5001}"
BACKEND_ONLY=0
REPLAY=0
RECORD_ONLY=0
SESSION_DIR=""
SESSION_ROOT=""

usage() {
    cat <<'EOF'
用法: ./run_web_debug.sh [选项]

  默认                真实使用（相机 + 串口 + 实时预览；录制由网页手动开始）
  --auto-record       启动即开始录制（可与默认预览模式组合）
  --record-only       无预览录制模式：不启动预览界面/Flask，按配置参数自动录制
  --source <spec>     模拟视频源: synthetic | synthetic:none | image:<path> | video:<path>
  --no-serial         不打开串口（模拟发送）
  --no-gui            无头运行（不创建 OpenCV 窗口）
  --no-record         关闭录制
  --record-root <dir> 录制会话根目录
  --no-web-debug      关闭实时预览
  --backend-only      只启动 Flask 实时后端
  --replay            只启动 M2 离线回放（默认 recordings/）
  --session <dir>     --replay 时指定单个会话目录
  --session-root <dir> --replay 时指定会话根目录
  -h, --help          显示本帮助

示例:
  ./run_web_debug.sh                                          # 真实使用（预览，录制手动开始）
  ./run_web_debug.sh --auto-record                            # 预览 + 启动即录制
  ./run_web_debug.sh --record-only                            # 无预览自动录制（现场一键录制）
  ./run_web_debug.sh --record-only --record-root /data/rec    # 无预览录制到指定目录
  ./run_web_debug.sh --source synthetic --no-serial --no-gui  # 无相机联调
  ./run_web_debug.sh --replay                                 # 离线回放
EOF
}

ARGS=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --backend-only)
            BACKEND_ONLY=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --replay)
            REPLAY=1
            shift
            ;;
        --record-only)
            RECORD_ONLY=1
            shift
            ;;
        --auto-record)
            ARGS+=("--auto-record")
            shift
            ;;
        --session)
            SESSION_DIR="${2:-}"
            shift 2 || shift
            ;;
        --session-root)
            SESSION_ROOT="${2:-}"
            shift 2 || shift
            ;;
        --source)
            spec="${2:-}"
            shift 2 || shift
            # image:/video: 的相对路径按项目根目录解析（main 在 build/ 目录下运行）
            case "$spec" in
                image:*|video:*)
                    kind="${spec%%:*}"
                    path="${spec#*:}"
                    case "$path" in
                        /*) ;;
                        *) path="$ROOT_DIR/$path" ;;
                    esac
                    ARGS+=("--source" "$kind:$path")
                    ;;
                *)
                    ARGS+=("--source" "$spec")
                    ;;
            esac
            ;;
        *)
            ARGS+=("$1")
            shift
            ;;
    esac
done

# ---- 0. M2 离线回放模式（独立服务，不启动视觉主程序/实时后端）----
if [ "$REPLAY" -eq 1 ]; then
    PLAYBACK_ARGS=()
    to_abs() {
        case "$1" in
            /*) printf '%s' "$1" ;;
            *) printf '%s' "$ROOT_DIR/$1" ;;
        esac
    }
    if [ -n "$SESSION_DIR" ]; then
        PLAYBACK_ARGS=(--dir "$(to_abs "$SESSION_DIR")")
    elif [ -n "$SESSION_ROOT" ]; then
        PLAYBACK_ARGS=(--root "$(to_abs "$SESSION_ROOT")")
    else
        PLAYBACK_ARGS=(--root "$ROOT_DIR/recordings")
    fi
    echo "[run_web_debug] 启动 M2 离线回放: http://$WEB_HOST:$REPLAY_PORT/ (会话: ${PLAYBACK_ARGS[*]})"
    exec python3 "$ROOT_DIR/debug/web/playback.py" \
        --host "$WEB_HOST" --port "$REPLAY_PORT" \
        --live-url "http://$WEB_HOST:$WEB_PORT/" "${PLAYBACK_ARGS[@]}"
fi

# ---- 0.2 无预览录制模式（--record-only）----
# 不启动 Flask / 预览界面，主程序按 config/debug.yaml 的参数自动开始录制（视频 + 遥测 + 事件）。
# 适合现场"一键录制"：参数使用配置文件中已保存的值（M3 网页保存后重启即生效）。
if [ "$RECORD_ONLY" -eq 1 ]; then
    if [ ! -x "$BUILD_DIR/main" ]; then
        echo "[run_web_debug] 未找到 build/main，先构建..."
        cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 1
        nice -n 19 cmake --build "$BUILD_DIR" -j2 || exit 1
    fi
    echo "[run_web_debug] 无预览录制模式：不启动实时预览/Flask，按配置参数自动录制"
    RECORD_ROOT_DISPLAY=""
    for ((i = 0; i < ${#ARGS[@]}; i++)); do
        if [ "${ARGS[$i]}" = "--record-root" ] && [ $((i + 1)) -lt ${#ARGS[@]} ]; then
            RECORD_ROOT_DISPLAY="${ARGS[$((i + 1))]}"
        fi
    done
    if [ -z "$RECORD_ROOT_DISPLAY" ]; then
        RECORD_ROOT_DISPLAY=$(awk '/^recording:/{f=1} f && /root:/{print $2; exit}' "$ROOT_DIR/config/debug.yaml" 2>/dev/null | tr -d '"')
        [ -n "$RECORD_ROOT_DISPLAY" ] && RECORD_ROOT_DISPLAY="$RECORD_ROOT_DISPLAY（config，相对项目根）"
    fi
    echo "[run_web_debug] 录制根目录: ${RECORD_ROOT_DISPLAY:-未知}"
    (
        cd "$BUILD_DIR" && ./main "${ARGS[@]}" --no-gui --no-web-debug --record --auto-record
    )
    MAIN_RC=$?
    echo "[run_web_debug] 视觉主程序退出 (code=$MAIN_RC)"
    exit "$MAIN_RC"
fi

# ---- 1. 启动 Flask 调试后端 ----
echo "[run_web_debug] 启动调试后端: http://$WEB_HOST:$WEB_PORT/ (数据口 $DATA_HOST:$DATA_PORT)"
python3 "$ROOT_DIR/debug/web/app.py" \
    --host "$WEB_HOST" --port "$WEB_PORT" \
    --data-host "$DATA_HOST" --data-port "$DATA_PORT" \
    --playback-url "http://$WEB_HOST:$REPLAY_PORT/" \
    > "$LOG_DIR/flask.log" 2>&1 &
FLASK_PID=$!
echo "$FLASK_PID" > "$LOG_DIR/flask.pid"

cleanup() {
    if kill -0 "$FLASK_PID" 2>/dev/null; then
        echo "[run_web_debug] 停止调试后端 (pid=$FLASK_PID)"
        kill "$FLASK_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

sleep 1
if ! kill -0 "$FLASK_PID" 2>/dev/null; then
    echo "[run_web_debug] 错误：调试后端启动失败，日志：$LOG_DIR/flask.log" >&2
    tail -5 "$LOG_DIR/flask.log" >&2 || true
    exit 1
fi
echo "[run_web_debug] 调试后端已启动 (pid=$FLASK_PID, 日志 $LOG_DIR/flask.log)"

if [ "$BACKEND_ONLY" -eq 1 ]; then
    echo "[run_web_debug] --backend-only：保持后端运行，Ctrl+C 退出"
    wait "$FLASK_PID"
    exit 0
fi

# ---- 2. 检查并构建主程序 ----
if [ ! -x "$BUILD_DIR/main" ]; then
    echo "[run_web_debug] 未找到 build/main，先构建..."
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 1
    nice -n 19 cmake --build "$BUILD_DIR" -j2 || exit 1
fi

# ---- 3. 启动视觉主程序 ----
# 若配置了海康 SDK 到系统路径之外，main 链接时已写入 rpath（见 CMakeLists.txt）
echo "[run_web_debug] 启动视觉主程序（附加参数：${ARGS[*]:-无}）"
(
    cd "$BUILD_DIR" && ./main "${ARGS[@]}"
)
MAIN_RC=$?
echo "[run_web_debug] 视觉主程序退出 (code=$MAIN_RC)"
exit "$MAIN_RC"

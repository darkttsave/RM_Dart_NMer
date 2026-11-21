# web_debug —— 调试、录制与回放适配层（M1 + M2）

本目录是 **DART-WEB-M1（实时调试）** 与 **DART-WEB-M2（数据录制与离线回放）** 的独立适配层实现。

设计原则（对应任务要求）：

| 要求 | 本实现 |
|---|---|
| 依赖少 | C++ 侧只用 POSIX socket + OpenCV；Flask 侧只用 Flask 标准能力（MJPEG / SSE），**不使用** SocketIO、numpy、pybind、嵌入式 Python |
| 侵入小 | C++ 侧新增 3 个独立模块（发布层 / 录制层 / 模拟帧源），`main.cpp` 只增加"参数解析 + 每帧提交 + 收尾"；视觉算法、相机采集、15 字节串口协议**均未改动** |
| 不引入 UMT | 与 Misaka 的 UMT/pybind 体系完全解耦，C++ 与 Flask 是两个独立进程 |
| 故障隔离 | 实时预览：Flask 未启动/浏览器断开只丢调试帧；录制：**完全不依赖 Flask/浏览器**，本地线程落盘 |
| 不伪造数据 | 全部字段来自实际算法输出、实际串口发送或真实统计；不存在的字段输出 `null` |
| 录制优先 | 录制覆盖每个实际完成视觉处理的帧（后台线程 + 有界队列），预览帧率不影响录制 |

## 1. 架构与数据流

```
┌──────────────────── C++ 视觉主程序（原有主链路，单线程主循环）─────────────────────┐
│ camera.getimage ─> GreenDetect::detectGreenTarget() ─> VirtualSerial::sendCommand() │
│                              │                                                     │
│        ┌─────────────────────┴────────────────────┐                                │
│        ▼                                          ▼                                │
│  DebugTelemetry.publish()                 Recorder.submitFrame()                   │
│  （单槽最新帧，非阻塞）                    （逐帧遥测 CSV + 视频有界队列）             │
│        │                                          │                                │
│        ▼                                          ├─ telemetry 线程：批量写 CSV       │
│  发送线程：JPEG+JSON                              ├─ video 线程：缩放+MJPG 编码+写 AVI │
│        │                                          │   （成功写入才记 video_index）    │
│        ▼                                          ▼                                │
│  TCP 127.0.0.1:5010                         recordings/<时间戳>/                     │
│                                                    ├── session.json                  │
│                                                    ├── config_snapshot/               │
│                                                    ├── debug_001.avi                  │
│                                                    ├── telemetry_001.csv              │
│                                                    └── video_index_001.csv            │
└────────────────────────────────────────────────────────────────────────────────────┘
        │                                                        │
        ▼                                                        ▼
┌─ Flask 实时后端（app.py, 独立进程）─┐            ┌─ Flask 回放服务（playback.py, 独立进程）─┐
│ /video_feed      MJPEG              │            │ /                 回放页面               │
│ /api/telemetry/stream  SSE          │            │ /api/sessions     会话列表              │
│ /api/status      快照               │            │ /api/session      遥测+索引全量          │
│ /api/recorder    录制状态/开始/停止  │──TCP 5011──▶│ /api/frame(_at)   视频帧 JPEG           │
└────────────────────────────────────┘  C++ 控制口  └──────────────────────────────────────────┘
```

## 2. 界面结构（M2-UI 重构后）

### 2.1 实时监测页（`/`：顶栏 / 左侧视频 / 右侧页签 / 底栏）

- **顶栏**：调试链路状态、**录制状态 chip + 开始/停止录制按钮（常驻）**、FPS、识别耗时、本帧串口输出、数据来源、时钟；
- **主区左侧**：实时视频（MJPEG），画面是页面主体，保留 LIVE / 分辨率 / 帧龄角标；
- **主区右侧两个页签**（同一时间只显示一个，避免信息堆叠）：
  - **状态面板**：引导灯识别（Tracking / 连续锁定 / 丢失计数 / 锁定进度 / 绿色像素 / 候选轮廓）、目标与偏差（**未检测到目标时坐标显示 `--`，不把算法占位值 (0,0) 当成测量结果**）、视觉输出与串口（turnFlag / distanceFlag / clearFlag / 本帧是否发送 / 实际发送值）、调试链路（接收帧率 / 帧龄 / 累计接收 / 数据来源）；
  - **实时曲线**：字段选择（8 个可选字段，默认 4 条）、时间窗口（5/10/30/60 s）、暂停 / 继续、清空；每条曲线**独立纵轴**，切换页签后自动重绘，图表高度随右侧可用高度自适应；
- **底栏**：会话名、录制统计（video 帧 / 遥测行）、队列丢弃与写入错误（**有错误时常显，不折叠**）、调试链路摘要，以及 `录制详情` 折叠浮层（会话目录 / 帧数 / 丢弃 / 错误）。

后端接口（M1 起不变）：

- `GET /video_feed`：MJPEG 实时画面（带识别标注）；
- `GET /api/telemetry/stream`：SSE 遥测样本（状态面板 + 曲线）；
- `GET /api/status`、`GET /api/meta`、`GET /healthz`；
- `GET/POST /api/recorder`（M2 新增）：转发到 C++ 本机录制控制口，查询/开始/停止录制；
- **预览帧率**：`config/debug.yaml → web_debug.publish_fps`
  - `0`（默认）：不主动限速，跟随算法处理帧率（实测约 30 fps）；
  - `>0`：主动限速；无新帧时仍阻塞等待，不会忙循环。

### 2.2 历史回放页（`playback.py` 独立服务：顶栏 / 左侧视频 / 右侧三页签 / 底部播放控制）

- **顶栏**：只读回放模式、会话选择、会话状态、数据来源、时长、视频缺失提示、返回实时调试台；
- **主区右侧三个页签**：
  - **历史曲线**：字段选择、时间窗口（5/10/20/60 s / 全会话）、播放头（红色竖线 = 当前回放时刻）；
  - **当前帧数据**：原始处理帧号 / 会话内时刻 / 目标坐标 / 偏差 / Tracking / 转向输出 / 本帧是否发送及实际发送值 / 算法 FPS 与识别耗时 / 时间与图像（视频帧索引、源帧号、绝对时间戳、图像尺寸与中心）；
  - **会话信息**：录制时间与时长、数据来源、帧与丢帧统计、录制错误、时间戳来源、**时间关联精度**、**配置快照**（`config_snapshot/runtime_params.json` 的运行时参数）；
- **底部**：播放 / 暂停、逐帧前后、±1 s、播放速度、按帧号定位、**统一时间轴（始终可见）**；
- **视频定位始终使用 `video_index_001.csv` 的实际帧时间索引，不使用视频标称 FPS。**

界面样式集中在 `web_debug/static/dashboard.css`（两个页面共用，保证录制/回放视觉一致）。

## 3. M2：数据录制

### 3.1 录制内容与格式

| 文件 | 内容 | 说明 |
|---|---|---|
| `debug_001.avi` | **识别后的调试画面**（`GreenDetect::getDebugImage()`，含检测标记/目标中心/调试可视化） | MJPG 编码；标称 `video_fps`（默认 30，仅播放器默认速度）；实际帧时间以视频索引为准 |
| `telemetry_001.csv` | 逐帧遥测（每个算法帧一行，不受预览帧率限制） | 29 列，见下 |
| `video_index_001.csv` | 视频帧 → 算法帧/时间戳 逐帧索引 | 只记录确认写入成功的视频帧 |
| `session.json` | 会话元数据（状态、时间、统计、文件清单、错误） | 原子替换写入；异常结束时 `status=recording` |
| `config_snapshot/debug.yaml` | 本次磁盘配置快照 | 原样复制 |
| `config_snapshot/runtime_params.json` | 运行时实际生效参数快照 | 来自主程序变量，与磁盘 YAML 可能不同 |

### 3.2 遥测 CSV 字段（`telemetry_001.csv`）

```
frame_id,timestamp_us,elapsed_us,detected,tracking,target_x,target_y,radius,
image_width,image_height,image_center_x,image_center_y,offset_x,offset_y,
turn_flag,distance_flag,clear_flag,tx_sent,tx_turn_flag,tx_distance_flag,tx_clear_flag,
lock_frames,required_frames,lost_frames,required_lost_frames,
green_pixels,candidate_count,fps,processing_time_ms
```

- `frame_id`：视觉处理帧编号（程序运行期连续递增，跨录制段保持连续）；
- `timestamp_us`：处理完成时刻的 `std::chrono::steady_clock` 微秒时间戳；
- `elapsed_us`：相对本次录制会话开始的微秒数（**回放使用该列，可跨机器**）；
- `clear_flag`：本帧实际串口发送值；`tx_sent=0` 时该列为空（不伪造）；
- `turn_flag / distance_flag`：原有算法输出（`distanceFlag` 不是实际距离）；
- 其他：`tracking`、锁定/丢失计数、绿色像素数、候选轮廓数、实测 fps、识别耗时。

### 3.3 视频索引（`video_index_001.csv`）

```
video_frame_index,source_frame_id,source_timestamp_us,source_elapsed_us
```

- 每成功写入一帧视频记录一行；
- `source_frame_id` 与遥测 `frame_id` 一一对应，`source_elapsed_us` 与遥测 `elapsed_us` 同基准；
- 回放**不依赖视频标称 FPS**，一律按 `source_elapsed_us` 定位。

### 3.4 时间基准（重要）

- 同一主机内使用 `steady_clock` 单调时钟；
- 无工业相机硬件时间戳，帧时间戳来源为 **`detectGreenTarget()` 处理完成时刻**（session.json 中已注明）；
- 跨机器回放只用 `elapsed_us`（相对会话开始），不依赖绝对单调时间。

### 3.5 录制控制

1. **启动参数**（不依赖 Flask）：
   - `--record` / `--no-record`：开关录制（覆盖配置）；
   - `--record-root <dir>`：会话根目录；
   - 配置项：`config/debug.yaml → recording.{enabled,auto_start,root,video_scale,video_fps,queue_capacity,control_port}`。
2. **本机控制口**（仅 `127.0.0.1`，不开放远程写入口）：
   ```bash
   echo '{"cmd":"status"}' | nc 127.0.0.1 5011
   echo '{"cmd":"stop"}'   | nc 127.0.0.1 5011
   echo '{"cmd":"start"}'  | nc 127.0.0.1 5011
   ```
3. **网页控制**：实时页面顶栏"开始录制/停止录制"按钮（`POST /api/recorder`）；Flask 仅转发，
   录制主体仍在 C++。状态显示包含视频帧数、遥测行数、丢弃与写入错误。
4. 每次开始录制创建独立目录，**多次录制不覆盖**；程序正常退出（SIGINT/SIGTERM/ESC）时关闭文件并更新 `session.json` 为 `completed`。

### 3.6 运行时行为

- 视觉线程：每帧格式化一行 CSV + 把 Mat 引用放入有界队列（微秒级），**不做磁盘 IO、不做视频缩放**；
- 视频线程：缩放（`video_scale`）+ MJPG 编码 + 写 AVI，**成功写入才写索引**；
- 队列满：丢帧并计数（`video_frames_dropped` / `telemetry_rows_dropped`），写入 `session.json`；
- 磁盘/编码错误：计数并写入 `errors`（不静默）；
- 录制不依赖 Flask/浏览器：Flask 未启动、浏览器关闭、Flask 崩溃都不影响录制。

## 4. M2：离线回放

独立 Flask 服务，只读会话目录，不连接相机、电控，不需要 C++ 主程序：

```bash
# 直接打开某个会话（可复制到任意目录/另一台电脑）
python3 web_debug/playback.py --dir recordings/2026-11-21_10-38-55 --port 5001

# 或列出会话根目录下的所有会话
python3 web_debug/playback.py --root recordings --port 5001
```

功能（显示逻辑参考 Misaka 后续改动的回放页面，独立实现）：

- 顶栏：`回放模式：只读 · 不向电控发送指令`、`返回实时调试台`链接（`--live-url`）、会话状态/来源/时长/缺失帧；
- 播放控制：播放/暂停、-1s/+1s、逐帧、**播放速度 0.25~4x**、时间轴拖动、按帧号定位（显示帧号范围）；
- **时间关联精度 chip**：`逐帧索引（精确）` + 说明 tooltip + 帧间隔中位数；
- 视频**全屏**（按钮 / 双击画面 / Esc 退出；Fullscreen API 不可用时页面内全屏兜底）；
- 曲线：**窗口选择 5/10/20/60s / 全会话** + **8 个可选字段**（默认 4 条）+ 窗口内样本数 + 播放头；
- 当前帧遥测面板（目标坐标/追踪/串口输出/fps/耗时）、视频缺失帧与不完整会话提示；
- 参数修改与人工事件：**M3 预留**（会话信息卡内说明）；
- 键盘快捷键：空格播放/暂停、←/→ ±1s。

启动参数增加 `--live-url <实时页地址>`（可选，用于"返回实时调试台"链接）。

接口：

| 接口 | 说明 |
|---|---|
| `GET /api/sessions` | 会话列表（含完整性、时长、缺失帧统计） |
| `GET /api/session?dir=` | 会话详情 + 全量遥测 + 视频索引 |
| `GET /api/frame?dir=&index=` | 指定视频帧 JPEG（响应头含 source_frame_id / elapsed_ms） |
| `GET /api/frame_at?dir=&t_ms=` | 按会话相对时间返回最近视频帧 |
| `GET /api/telemetry_at?dir=&t_ms=` | 按时间取遥测 |
| `GET /api/telemetry_by_frame?dir=&frame_id=` | 按帧号取遥测 |

`GET /api/session` 返回的 `summary` 还包括 `accurate/accuracy_note`（时间关联精度）、
`frame_id_min/max`（帧号范围）与 `config_snapshot`（M2-UI 页签 C 用的配置快照：文件清单 +
`runtime_params.json` 的运行时参数，只读展示、不参与时间关联）。

派生依赖：Python 3 + Flask + OpenCV（python3-opencv / opencv-python）。

> 注意：`cv2.VideoCapture` 非线程安全，回放服务把所有 OpenCV 解码/编码固定在一个单线程执行器中，
> 避免 Flask 多线程请求下的偶发解码失败（M2 联调时实际踩到并修复）。

## 5. 启动方式

```bash
# 实时：Flask + 主程序（默认录制 + 预览不限速）
./run_web_debug.sh

# 无工业相机联调（合成绿色引导灯视频源）
./run_web_debug.sh --source synthetic --no-serial --no-gui

# 关闭录制
./run_web_debug.sh --no-record

# 只启动 Flask（视觉程序另行启动）
./run_web_debug.sh --backend-only

# 离线回放（默认 recordings/）
./run_web_debug.sh --replay
./run_web_debug.sh --replay --session recordings/2026-11-21_10-38-55
./run_web_debug.sh --replay --session-root /path/to/copied/recordings
```

主程序参数（全部可选，不指定时与原有行为一致）：

| 参数 | 说明 |
|---|---|
| `--source <spec>` | 模拟视频源：`synthetic` / `synthetic:none` / `image:<path>` / `video:<path>` |
| `--no-serial` | 不打开串口，模拟发送（无电控环境测试） |
| `--no-gui` | 不创建 OpenCV 窗口（无头运行） |
| `--record` / `--no-record` | 录制开关（覆盖配置） |
| `--record-root <dir>` | 录制会话根目录 |
| `--web-debug` / `--no-web-debug` | 实时预览开关（覆盖配置） |
| `-h` / `--help` | 帮助 |

## 6. 自检

```bash
# M1 实时链路（视觉程序 + Flask 都在运行）：期望 20/20
python3 test/test_web_debug_checks.py

# M2 录制性能对照（三种配置：不录制不预览 / 仅录制 / 录制+预览）
python3 test/test_recording_bench.py --seconds 20
python3 test/test_recording_bench.py --seconds 20 --configs record_with_preview --flask

# 离线降级（只启动 Flask）
python3 test/test_web_debug_checks.py --offline

# M2-UI 页面截图与功能回归（需要实时 Flask + 回放服务都在运行）
NODE_PATH=$(npm root -g) node tools/screenshot_m2ui.js live-status http://127.0.0.1:5000/ /tmp/live.png 1920 1080
NODE_PATH=$(npm root -g) node tools/screenshot_m2ui.js replay-charts http://127.0.0.1:5001/ /tmp/replay.png 1920 1080
NODE_PATH=$(npm root -g) node tools/regression_m2ui.js http://127.0.0.1:5000/ http://127.0.0.1:5001/
```

## 7. 故障隔离行为（实测）

| 场景 | 实时预览 | 本地录制 |
|---|---|---|
| Flask 未启动 | C++ 每秒退避重连，调试帧丢弃 | 正常录制 |
| Flask 中途退出 | 自动重连 | 正常录制（实测：6s 时杀 Flask，剩余 9s 录制完整） |
| 浏览器断开 | 生成器结束，无影响 | 正常录制 |
| 视觉程序退出 | Flask 显示数据过期 | 文件正常关闭，session.json 更新 |

## 8. 与 Misaka 调试工程的关系

- 参考 Misaka A1 的**界面信息结构**、A2/A3/A5 的**功能范围**，M2 的录制状态/控制与回放交互
  进一步参考了 Misaka 工作区**后续未提交改动**的显示逻辑（只读查看）；但前端/后端均为本项目
  独立实现（HTML/CSS/原生 JS + Canvas，无第三方前端库、无外部 CDN），未复制其代码；
- **未复制** Misaka 的代码（其依赖 UMT/pybind/嵌入式 Python，与本项目架构不兼容）；
- Misaka 工程全程只读，未做任何修改。

## 9. 后续扩展（M3+）

- 电控遥测：在 `telemetry_001.csv` 追加列 / 新增独立 CSV，录制层与回放层无需改协议；
- 参数在线修改与事件记录（参考 Misaka A4）：新增 `events.csv` + 回放时间轴标记；
- 原始画面 + 调试画面双路录制：`video_scale` 之外增加 `record_raw` 选项（帧源已在 `submitFrame` 中集中）；
- 长会话回放性能：遥测分页 / 视频帧预取；
- 压缩策略：大分辨率 MJPG 体积较大时，可评估 FFmpeg/硬件编码（本轮按要求不做提前优化）。

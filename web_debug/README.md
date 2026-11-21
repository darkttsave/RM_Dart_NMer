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

## 2. 界面结构（M2-UI 重构后 + M3 参数页签 + M4 双调试台）

### 2.1 实时监测页（`/`：顶栏 / 左侧视频 / 右侧页签 / 底栏）

- **顶部工作台导航（M4）**：`视觉调试 | 机械与电控 | 历史回放`；页面切换只换 Flask 页面，
  不重启 C++ 主程序、不中断录像；
- **顶栏**：调试链路状态、**录制状态 chip + 开始/停止录制按钮（常驻）**、FPS、识别耗时、本帧串口输出、数据来源、时钟；
  （默认启动不自动录制，点「开始录制」开始；启动即录制用 `--auto-record`）
- **主区左侧**：实时视频（MJPEG），画面是页面主体，保留 LIVE / 分辨率 / 帧龄角标；
- **主区右侧三个页签**（同一时间只显示一个，避免信息堆叠）：
  - **状态面板**：引导灯识别（Tracking / 连续锁定 / 丢失计数 / 锁定进度 / 绿色像素 / 候选轮廓）、目标与偏差（**未检测到目标时坐标显示 `--`，不把算法占位值 (0,0) 当成测量结果**）、视觉输出与串口（turnFlag / distanceFlag / clearFlag / 本帧是否发送 / 实际发送值）、调试链路（接收帧率 / 帧龄 / 累计接收 / 数据来源）；
  - **实时曲线**：字段选择（8 个可选字段，默认 4 条）、时间窗口（5/10/30/60 s）、暂停 / 继续、清空；每条曲线**独立纵轴**，切换页签后自动重绘，图表高度随右侧可用高度自适应；
  - **参数调整（M3）**：按颜色识别 / 候选目标筛选 / 追踪与连续帧 / 转向与距离 / 相机分组，显示「运行值 / 配置文件值」，支持应用（热更新）与保存（写 YAML），详见 §3；
- **水平偏差标柱（M4）**：位于「目标与偏差」面板内，显示 `offset_x` 相对图像中心的横向偏差
  （中心 0 线、±400 px 量程、指针与数值、左右方向、`turn_threshold` 对准区间、当前 `turnFlag`）；
  阈值取自 C++ 实际运行参数（参数调整后自动跟随）；未检测到有效目标时不显示指针、显示降级提示，详见 §4.3；
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
- **主区右侧四个页签**：
  - **历史曲线**：字段选择、时间窗口（5/10/20/60 s / 全会话）、播放头（红色竖线 = 当前回放时刻）；
  - **当前帧数据**：原始处理帧号 / 会话内时刻 / 目标坐标 / 偏差 / Tracking / 转向输出 / 本帧是否发送及实际发送值 / 算法 FPS 与识别耗时 / 时间与图像（视频帧索引、源帧号、绝对时间戳、图像尺寸与中心）；
  - **会话信息**：录制时间与时长、数据来源、帧与丢帧统计、录制错误、时间戳来源、**时间关联精度**、**配置快照**（`config_snapshot/runtime_params.json` 的运行时参数）、**参数修改事件数**；
  - **参数事件（M3）**：事件列表（时间 / 参数 / 旧值→新值 / 来源 / 结果），点击跳转时间轴、视频帧与曲线播放头，曲线以黄色虚线标记事件位置，详见 §3.5；
- **底部**：播放 / 暂停、逐帧前后、±1 s、播放速度、按帧号定位、**统一时间轴（始终可见）**；
- **视频定位始终使用 `video_index_001.csv` 的实际帧时间索引，不使用视频标称 FPS。**

界面样式集中在 `web_debug/static/dashboard.css`（两个页面共用，保证录制/回放视觉一致）。

### 2.3 机械与电控调试台（`/mechanical`，M4）

- 顶部同一套工作台导航；无相机视频区域；
- 摘要行：当前目标编号 / 当前飞镖编号 / 运行阶段（电控）→ **待接入**；通信在线 → 视觉链路真实状态 + 电控链路未接入说明；
- **左侧（主要观测区）**：电控遥测曲线——顶部按用途分组可选（角度与位置 / 速度与运动状态 / 蓄力相关参数 / 其他遥测）；
  电控遥测未接入时绘图区显示「待接入」，不绘制模拟曲线；
- **右侧页签**（按键切换，同一时间只显示一个）：
  - **状态**：电机状态、工作模式、运行阶段、目标/飞镖编号（待接入）+ 模拟数据预览开关（SIM 标识）；
  - **参数**：Yaw 目标/实际、蓄力目标/实际（待接入）+ 视觉侧关联（只读真实数据）；
  - **运行**：会话运行状态（录制/会话/帧数/遥测/事件与人工记录数/链路，真实数据）；
  - **人工记录**：人工调试记录（录制期间写入当前会话 `events.jsonl`，类型 `manual_note`、来源 `manual`）；
- 字段映射与真实/模拟/人工/待接入分类详见 §4。

## 3. M3：运行时参数管理与参数事件

### 3.1 架构与数据流

```text
浏览器「参数调整」页签
      │  GET/POST /api/params（Flask 只做基础类型校验并转发）
      ▼
C++ 本机控制口 127.0.0.1:5011（复用 M2 控制链路，不引入新的通信框架）
      │  params / params.apply / params.save
      ▼
RuntimeParams（C++ 运行时参数服务，唯一权威状态）
      │  权威校验 + 加锁更新（整组校验通过才生效，失败保留原参数）
      ▼
视觉主循环：每帧取一次完整参数快照 → GreenDetect / 相机 SDK
      │
      └─ 每次实际生效的修改 → Recorder::appendParameterEvent（录制中追加 events.jsonl）
```

- **C++ 运行时状态是实际生效值的权威来源**：网页只发送期望值，界面显示值来自 C++ 返回；
- 视觉线程每帧 `snapshot()` 一次，同一帧处理过程中不会混用修改前后的参数；
- 应用与保存分离：应用立即改运行值；保存写 YAML 供下次启动。

### 3.2 网页可调整参数（20 项，均支持热更新）

| 分组 | 参数 | 类型 | 范围 |
|---|---|---|---|
| 颜色识别 | `bright_thresh` | int | 0–255 |
| 颜色识别 | `green_weight_thresh` | float | 0–1 |
| 候选目标筛选 | `min_area` / `max_area` | int | 1–1000000 |
| 候选目标筛选 | `min_radius` / `max_radius` | int | 1–500 / 1–1000 |
| 候选目标筛选 | `min_circularity` | float | 0–1 |
| 候选目标筛选 | `min_aspect_ratio` / `max_aspect_ratio` | float | 0.01–20 |
| 候选目标筛选 | `kernel_size` / `open_iterations` / `close_iterations` | int | 1–31 / 0–10 / 0–10 |
| 追踪与连续帧 | `required_frames` / `required_lost_frames` | int | 1–100 / 1–1000 |
| 追踪与连续帧 | `max_center_dist` | float | 1–5000 |
| 转向与距离 | `turn_threshold` / `dist_far_thresh` / `dist_near_thresh` | float | 0–2000 / 0–5000 / 0–5000 |
| 相机 | `exposure` / `gain` | int | 10–1000000 / 0–100 |

只读/需重启（网页不开放修改）：串口 `port`/`baudrate`/`watchdog`、`web_debug.*`、`recording.*`、
`camera.serial_number`、`debug_setting.*`、`runtime_logging.*`。

### 3.3 应用 / 保存 / 读取

| 操作 | 接口 | 行为 |
|---|---|---|
| 读取 | `GET /api/params` | 返回 `running`（实际运行值）/ `saved`（YAML 值）/ `dirty` / `params` 元数据 / `config_path` |
| 应用 | `POST /api/params {"cmd":"apply","values":{...}}` | C++ 校验类型/范围/交叉约束；**整组失败则整组不生效**；返回实际生效后的值 |
| 保存 | `POST /api/params {"cmd":"save"}` | 把当前运行值写入 `config/debug.yaml`；文本级替换仅改目标值（**保留注释/顺序/无关配置**），临时文件 + 原子替换，写后 yaml-cpp 解析校验 |

交叉约束：`min_radius ≤ max_radius`、`min_area ≤ max_area`、`min_aspect_ratio ≤ max_aspect_ratio`、
`dist_near_thresh ≤ dist_far_thresh`。

GUI Trackbar：保留原有调参能力，但运行值只有 `RuntimeParams` 一份——拖动 Trackbar 写回参数服务
（来源 `trackbar`），网页修改后反向刷新 Trackbar 显示；Trackbar 范围只影响显示，不会把超范围值
写回运行参数（避免滑块精度/范围截断配置值）。

### 3.4 参数事件（`events.jsonl`）

录制期间每次实际生效的修改追加一行（会话目录内，与遥测同一时间基准）：

```json
{"type":"parameter_changed","elapsed_us":4894694,"frame_id":146,"source":"web",
 "parameter":"bright_thresh","old_value":80,"new_value":110,"result":"applied"}
```

- `elapsed_us`：相对会话开始（`steady_clock`），与 `telemetry_001.csv` 的 `elapsed_us` 同一基准；
- `frame_id`：修改生效时最近完成帧的算法帧号；
- `source`：`web`（网页）或 `trackbar`（GUI 滑条）；
- 只记录实际生效的修改（`result=applied`）；未录制时修改只更新运行值，不写事件；
- 会话开始时保存 `config_snapshot/runtime_params.json`（M3 起为当时实际运行值），
  「会话开始快照 + 录制期间事件 = 历史时刻参数状态」；`session.json` 的 `counts.parameter_events`
  记录事件数。

### 3.5 回放参数事件

回放页「参数事件」页签显示时间 / 参数 / 旧值→新值 / 来源 / 结果；点击事件后时间轴、视频帧与
曲线播放头同步跳转，历史曲线上以黄色虚线标记事件位置。旧 M2 会话没有 `events.jsonl` 时正常回放，
显示「该会话无参数修改事件」。回放页只读，不会修改 C++ 实时运行参数。

## 4. M4：双调试台与偏差标柱

### 4.1 统一导航

| 页面 | 路径 | 说明 |
|---|---|---|
| 视觉调试 | `/` | 实时视频、状态面板、实时曲线、参数调整（M1–M3 能力保持不变） |
| 机械与电控 | `/mechanical` | 无视频区；电控字段映射（待接入）、视觉侧只读关联、人工调试记录 |
| 历史回放 | 独立服务（默认 `:5001`） | 复用 M2/M3 的会话数据、参数事件与人工记录 |

两个工作台由同一个 Flask 服务渲染（`app.py` 增加 `/mechanical` 路由），复用同一份运行状态数据，
切换页面不重启 C++ 进程、不中断录制（实测跳转期间录像帧数持续增长）。

### 4.2 电控字段映射表（接口预留）

参考 Misaka `app/board_adapter.py` 的字段清单；**只有队内代码/协议/数据来源能证明的字段才标记为真实接入**。
当前队内视觉程序通过 15 字节串口**单向发送**控制指令（`VirtualSerial` 仅发送），未接收电控遥测，
因此下列字段均标记「待接入」，由 `/api/board` 统一定义与返回状态：

| 字段 id | 界面标签 | 类型 | 数据来源约定 | 状态 |
|---|---|---|---|---|
| `target_id` | 当前目标编号 | int | 电控上报 | 待接入 |
| `dart_number` | 当前飞镖编号 | int | 电控上报（视觉程序不产生镖体编号） | 待接入 |
| `mode` | 电控工作模式 | enum | 电控上报 | 待接入 |
| `stage` | 电控运行阶段 | enum | 电控上报 | 待接入 |
| `yaw_target` | Yaw 目标值 | float(deg) | 电控上报 | 待接入 |
| `yaw_actual` | Yaw 实际值 | float(deg) | 电控上报 | 待接入 |
| `charge_target` | 蓄力目标值 | float | 电控上报 | 待接入 |
| `charge_actual` | 蓄力实际值 | float | 电控上报 | 待接入 |
| `motor_state` | 电机状态 | enum | 电控上报 | 待接入 |
| `comm_online` | 通信在线状态 | status | 视觉调试链路真实状态 + 电控链路状态 | 部分真实 |

视觉侧只读关联（真实，来源 C++ 视觉算法）：视觉检测有效、`offset_x`、`turnFlag`、`distanceFlag`、
`clearFlag`、本帧串口发送、实际发送 T/D/C。

分类约定：

- **真实接入**：数据来自 C++ 运行状态或调试帧（视觉侧关联、通信在线、会话运行状态）；
- **模拟数据**：机械页「模拟数据预览」开关仅为界面演示，字段带 `SIM` 标识、**不写入录制**、不冒充实测；
- **人工记录**：机械/电控调试备注，写入会话 `events.jsonl`（`type=manual_note`, `source=manual`）；
- **待接入**：协议未确认或无数据来源的字段，界面显示「待接入」，不生成模拟曲线。

### 4.3 水平偏差标柱（视觉调试台）

- 数据：当前 `offset_x`（C++ 调试帧）+ `turn_threshold`（`/api/params` 实际运行值）+ `turnFlag`；
- 显示：中央 0 线 = 图像中心；量程 ±400 px（超出时指针贴边并标注真实值）；指针数值、方向（左偏/右偏/对准）、
  绿色对准区间（`|offset_x| < turn_threshold`，按真实数值比例绘制，过窄时最小可见宽度）；
- 未检测到有效目标：不绘制指针，显示「未检测到有效目标 · 不显示偏差」，数值显示 `--`；
- 参数调整后：阈值位置与文本随实际运行值刷新（标柱每 3 s 读取一次 `/api/params`）；
- 仅显示，不改变算法输出、转向判断逻辑或 15 字节协议。

> Misaka 参考差异：Misaka 实际以「视觉偏差曲线」（`detector/yaw_diff_px`）展示偏差，并未实现竖线标柱组件；
> 本轮按用户要求的"竖线标柱"视觉效果独立实现（横向刻度 + 竖线指针 + 阈值区间），未复制其代码。

### 4.4 接口

| 接口 | 说明 |
|---|---|
| `GET /mechanical` | 机械与电控调试台页面（无视频区） |
| `GET /api/board` | 字段映射状态（10 项电控字段）+ 视觉侧只读数据（9 项）+ 运行状态 |
| `POST /api/board/note` | 人工调试记录：录制期间写入当前会话 `events.jsonl`（未录制时明确报错，不伪造成功） |
| `GET /api/params` | 偏差标柱读取 `turn_threshold` 实际运行值（M3 接口） |

人工记录事件结构（追加到会话 `events.jsonl`，与遥测同一时间基准）：

```json
{"type":"manual_note","elapsed_us":3013906,"frame_id":1003,"source":"manual","note":"Yaw 机构紧固后复测","result":"recorded"}
```

回放页「事件记录」页签同时展示参数修改事件（`parameter_changed`）与人工记录（`manual_note`）；
人工记录以「人工记录」标签 + 备注文本显示，可点击跳转时间轴/视频帧/曲线播放头。

## 5. M2：数据录制

### 5.1 录制内容与格式

| 文件 | 内容 | 说明 |
|---|---|---|
| `debug_001.avi` | **识别后的调试画面**（`GreenDetect::getDebugImage()`，含检测标记/目标中心/调试可视化） | MJPG 编码；标称 `video_fps`（默认 30，仅播放器默认速度）；实际帧时间以视频索引为准 |
| `telemetry_001.csv` | 逐帧遥测（每个算法帧一行，不受预览帧率限制） | 29 列，见下 |
| `video_index_001.csv` | 视频帧 → 算法帧/时间戳 逐帧索引 | 只记录确认写入成功的视频帧 |
| `session.json` | 会话元数据（状态、时间、统计、文件清单、错误） | 原子替换写入；异常结束时 `status=recording` |
| `config_snapshot/debug.yaml` | 本次磁盘配置快照 | 原样复制 |
| `config_snapshot/runtime_params.json` | 运行时实际生效参数快照 | 来自主程序变量，与磁盘 YAML 可能不同 |

### 5.2 遥测 CSV 字段（`telemetry_001.csv`）

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

### 5.3 视频索引（`video_index_001.csv`）

```
video_frame_index,source_frame_id,source_timestamp_us,source_elapsed_us
```

- 每成功写入一帧视频记录一行；
- `source_frame_id` 与遥测 `frame_id` 一一对应，`source_elapsed_us` 与遥测 `elapsed_us` 同基准；
- 回放**不依赖视频标称 FPS**，一律按 `source_elapsed_us` 定位。

### 5.4 时间基准（重要）

- 同一主机内使用 `steady_clock` 单调时钟；
- 无工业相机硬件时间戳，帧时间戳来源为 **`detectGreenTarget()` 处理完成时刻**（session.json 中已注明）；
- 跨机器回放只用 `elapsed_us`（相对会话开始），不依赖绝对单调时间。

### 5.5 录制控制

1. **启动参数**（不依赖 Flask）：
   - `--record` / `--no-record`：开关录制（覆盖配置）；
   - `--auto-record` / `--no-auto-record`：是否启动即开始录制（覆盖配置 `recording.auto_start`）；
   - `--record-root <dir>`：会话根目录；
   - 配置项：`config/debug.yaml → recording.{enabled,auto_start,root,video_scale,video_fps,queue_capacity,control_port}`；
   - **默认 `auto_start: false`**：预览调试启动后由网页「开始录制」按钮控制；
     需要启动即录制时用 `--auto-record`，或直接用无预览录制模式 `./run_web_debug.sh --record-only`。
2. **本机控制口**（仅 `127.0.0.1`，不开放远程写入口）：
   ```bash
   echo '{"cmd":"status"}' | nc 127.0.0.1 5011
   echo '{"cmd":"stop"}'   | nc 127.0.0.1 5011
   echo '{"cmd":"start"}'  | nc 127.0.0.1 5011
   ```
3. **网页控制**：实时页面顶栏"开始录制/停止录制"按钮（`POST /api/recorder`）；Flask 仅转发，
   录制主体仍在 C++。状态显示包含视频帧数、遥测行数、丢弃与写入错误。
4. 每次开始录制创建独立目录，**多次录制不覆盖**；程序正常退出（SIGINT/SIGTERM/ESC）时关闭文件并更新 `session.json` 为 `completed`。

### 5.6 运行时行为

- 视觉线程：每帧格式化一行 CSV + 把 Mat 引用放入有界队列（微秒级），**不做磁盘 IO、不做视频缩放**；
- 视频线程：缩放（`video_scale`）+ MJPG 编码 + 写 AVI，**成功写入才写索引**；
- 队列满：丢帧并计数（`video_frames_dropped` / `telemetry_rows_dropped`），写入 `session.json`；
- 磁盘/编码错误：计数并写入 `errors`（不静默）；
- 录制不依赖 Flask/浏览器：Flask 未启动、浏览器关闭、Flask 崩溃都不影响录制。

## 6. M2：离线回放

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

## 7. 启动方式

```bash
# 默认：实时预览（相机 + 串口）；录制由网页按钮手动开始（config: recording.auto_start=false）
./run_web_debug.sh

# 实时预览 + 启动即开始录制
./run_web_debug.sh --auto-record

# 无预览录制模式：不启动预览界面/Flask，按配置参数（含网页保存值）自动录制
./run_web_debug.sh --record-only
./run_web_debug.sh --record-only --record-root /data/dart_recordings   # 指定录制目录

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

> `--record-only`：适合现场"一键录制"——不启动 Flask/预览，主程序直接按 `config/debug.yaml`
> 中已保存的参数（视觉参数、相机曝光/增益、录制目录等）自动开始录制视频 + 逐帧遥测 + 参数事件，
> Ctrl+C / SIGINT 正常退出并关闭会话。

主程序参数（全部可选，不指定时与原有行为一致）：

| 参数 | 说明 |
|---|---|
| `--source <spec>` | 模拟视频源：`synthetic` / `synthetic:none` / `image:<path>` / `video:<path>` |
| `--no-serial` | 不打开串口，模拟发送（无电控环境测试） |
| `--no-gui` | 不创建 OpenCV 窗口（无头运行） |
| `--record` / `--no-record` | 录制开关（覆盖配置） |
| `--auto-record` / `--no-auto-record` | 是否启动即录制（覆盖配置 `recording.auto_start`，默认不自动录制） |
| `--record-root <dir>` | 录制会话根目录 |
| `--web-debug` / `--no-web-debug` | 实时预览开关（覆盖配置） |
| `-h` / `--help` | 帮助 |

## 8. 自检

```bash
# M1 实时链路（视觉程序 + Flask 都在运行）：期望 20/20
python3 test/test_web_debug_checks.py

# M2 录制性能对照（三种配置：不录制不预览 / 仅录制 / 录制+预览）
python3 test/test_recording_bench.py --seconds 20
python3 test/test_recording_bench.py --seconds 20 --configs record_with_preview --flask

# 离线降级（只启动 Flask）
python3 test/test_web_debug_checks.py --offline

# M4 双工作台与偏差标柱回归（需要 main + Flask + 回放服务运行中）
NODE_PATH=$(npm root -g) node tools/regression_m4.js http://127.0.0.1:5000/ http://127.0.0.1:5001/ /tmp/m4ui

# M3 参数管理自检（需要 main + Flask 运行中；保存测试请用隔离配置副本）
python3 test/test_m3_params.py --config /tmp/m3test/config/debug.yaml --session /tmp/m3test/recordings/<会话>
NODE_PATH=$(npm root -g) node tools/regression_m3.js http://127.0.0.1:5000/ http://127.0.0.1:5001/ /tmp/m3ui

# M2-UI 页面截图与功能回归（需要实时 Flask + 回放服务都在运行）
NODE_PATH=$(npm root -g) node tools/screenshot_m2ui.js live-status http://127.0.0.1:5000/ /tmp/live.png 1920 1080
NODE_PATH=$(npm root -g) node tools/screenshot_m2ui.js replay-charts http://127.0.0.1:5001/ /tmp/replay.png 1920 1080
NODE_PATH=$(npm root -g) node tools/regression_m2ui.js http://127.0.0.1:5000/ http://127.0.0.1:5001/
```

## 9. 故障隔离行为（实测）

| 场景 | 实时预览 | 本地录制 |
|---|---|---|
| Flask 未启动 | C++ 每秒退避重连，调试帧丢弃 | 正常录制 |
| Flask 中途退出 | 自动重连 | 正常录制（实测：6s 时杀 Flask，剩余 9s 录制完整） |
| 浏览器断开 | 生成器结束，无影响 | 正常录制 |
| 视觉程序退出 | Flask 显示数据过期 | 文件正常关闭，session.json 更新 |

## 10. 与 Misaka 调试工程的关系

- 参考 Misaka A1 的**界面信息结构**、A2/A3/A5 的**功能范围**，M2 的录制状态/控制与回放交互
  进一步参考了 Misaka 工作区**后续未提交改动**的显示逻辑（只读查看）；但前端/后端均为本项目
  独立实现（HTML/CSS/原生 JS + Canvas，无第三方前端库、无外部 CDN），未复制其代码；
- **未复制** Misaka 的代码（其依赖 UMT/pybind/嵌入式 Python，与本项目架构不兼容）；
- Misaka 工程全程只读，未做任何修改。

## 11. 后续扩展（M5+）

- 电控遥测：在 `telemetry_001.csv` 追加列 / 新增独立 CSV，录制层与回放层无需改协议；
- 参数在线修改与事件记录（参考 Misaka A4）：新增 `events.csv` + 回放时间轴标记；
- 原始画面 + 调试画面双路录制：`video_scale` 之外增加 `record_raw` 选项（帧源已在 `submitFrame` 中集中）；
- 长会话回放性能：遥测分页 / 视频帧预取；
- 压缩策略：大分辨率 MJPG 体积较大时，可评估 FFmpeg/硬件编码（本轮按要求不做提前优化）。

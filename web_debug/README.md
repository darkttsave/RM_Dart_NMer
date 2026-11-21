# web_debug —— M1 Web 调试适配层

本目录是 **DART-WEB-M1** 的独立适配层实现：为队内 C++ 视觉程序接入 Web 调试能力。

设计原则（对应任务要求）：

| 要求 | 本实现 |
|---|---|
| 依赖少 | C++ 侧只用 POSIX socket + OpenCV；Flask 侧只用 Flask 标准能力（MJPEG / SSE），**不使用** SocketIO、numpy、pybind、嵌入式 Python |
| 侵入小 | `main.cpp` 仅新增「参数解析 + 发布点」两处；视觉算法、相机采集、15 字节串口协议**均未改动** |
| 不引入 UMT | 与 Misaka 的 UMT/pybind 体系完全解耦，C++ 与 Flask 是两个独立进程 |
| 故障隔离 | 视觉线程只做非阻塞的「单槽最新帧」拷贝；JPEG 编码、网络 IO 全部在后台线程；Flask 未启动/浏览器未连接/断开时自动丢帧并退避重连 |
| 不伪造数据 | 全部字段来自实际算法输出、实际串口发送或真实统计；不存在的字段（如未发送帧的 clearFlag）输出 `null` |

## 1. 架构与数据流

```
┌─────────────────────────── C++ 视觉主程序（原有主链路）──────────────────────────┐
│  camera.read  ──>  GreenDetect::detectGreenTarget()  ──>  VirtualSerial::sendCommand() │
│                              │                                                        │
│                    publish(DebugFrame)   ← 视觉线程，仅 1 次互斥锁 + cv::Mat 引用计数拷贝 │
│                              ▼                                                        │
│                 DebugTelemetry 后台线程（默认 20 fps）                                   │
│                 JPEG 编码 + JSON 组包 ──> TCP 127.0.0.1:5010                             │
└───────────────────────────────────────────────────────────────────────────────────────┘
                                         │
┌──────────────────────────── Flask（独立进程，web_debug/app.py）────────────────────────┐
│  FrameReceiver 线程（TCP 5010）──> 最新帧缓存 + 30s 曲线历史                              │
│        │                                                                               │
│        ├── GET /video_feed                MJPEG  实时视觉画面                            │
│        ├── GET /api/telemetry/stream      SSE    实时遥测样本（状态面板 + 曲线）           │
│        ├── GET /api/status                REST   一次性快照（初始化/自检）                │
│        └── GET /api/meta                  接口与 M2 预留说明                             │
└───────────────────────────────────────────────────────────────────────────────────────┘
                                         │
                              浏览器 http://127.0.0.1:5000/
```

**为什么用 TCP + 自定义帧，而不是共享内存 / 嵌入式 Flask：**

1. 共享内存需要额外锁与生命周期管理，且 Flask 无法天然感知生产者退出；
2. 嵌入式 Python（Misaka 的 `Py_Main` 方式）会把 Flask 生命周期绑进视觉进程，调试系统崩溃会直接影响主链路，不符合任务第五节；
3. 独立进程 + TCP 的故障隔离最彻底：Flask 挂了视觉程序只是丢帧；视觉程序退出 Flask 自动显示"数据过期"；
4. TCP 走本机回环，2 MB/s 级别的 JPEG 流量开销可忽略，且便于后续 M2 直接切换为远程查看。

## 2. 帧协议（C++ → Flask，小端）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | magic | `"DWDB"` |
| 4 | 1 | version | `1` |
| 5 | 1 | flags | 保留（0） |
| 6 | 2 | reserved | 保留（0） |
| 8 | 4 | json_len | 遥测 JSON 字节数 |
| 12 | 4 | jpeg_len | JPEG 字节数（可为 0） |
| 16 | json_len | json | UTF-8 JSON |
| 16+json_len | jpeg_len | jpeg | 调试画面（带识别标注，`GreenDetect::getDebugImage()`） |

单帧上限：JSON 256 KB / JPEG 8 MB（Flask 侧校验，超限断开重连）。

## 3. 调试数据字段

| 字段 | 来源 | 说明 |
|---|---|---|
| `seq` / `timestamp_ms` | 发布序号 / 系统时钟 | 帧序号与时间戳 |
| `source` / `source_label` | 主程序实际数据源 | `hikvision` / `synthetic` / `sim_image` / `sim_video` |
| `mode` | 运行模式 | 当前固定 `run`（M2 可扩展 replay 等） |
| `detected` | `TargetInfo.detected` | 是否检测到有效引导灯（连续帧确认后的追踪态） |
| `target_x` / `target_y` | `TargetInfo.center` | 目标中心坐标（像素） |
| `radius` | `TargetInfo.radius` | 目标半径（像素） |
| `image_width` / `image_height` | 当前帧尺寸 | 图像尺寸 |
| `image_center_x/y` | `GreenDetect::getImageCenter()` | 画面中心坐标 |
| `offset_x` / `offset_y` | `target - image_center` | 目标相对画面中心的偏差（像素） |
| `turn_flag` | `TargetInfo.turnFlag` | 原有转向输出：0=对准/停止，1=左转，2=右转 |
| `distance_flag` | `TargetInfo.distanceFlag` | 原有水平阶段信息（注意：**不是实际距离**）：1=远，2=中，3=对准/无目标 |
| `clear_flag` | 本帧实际发送帧 | 原有清除标志；**本帧未发送串口时为 `null`** |
| `tx.sent` | 实际发送结果 | 本帧是否向电控发送了 15 字节帧 |
| `tx.turn_flag/distance_flag/clear_flag` | 实际发送值 | 未发送时整个 `tx` 只有 `sent:false` |
| `fps` | 主循环实测 | 含采集/显示开销的真实处理帧率（EMA） |
| `processing_time_ms` | 主循环实测 | `detectGreenTarget()` 单帧耗时 |
| `tracking` / `lock_frames` / `required_frames` | `GreenDetect` 内部状态 | 连续帧确认状态与锁定进度 |
| `lost_frames` / `required_lost_frames` | `GreenDetect` 内部状态 | 丢失计数与丢失阈值 |
| `green_pixels` | 预处理统计 | 绿色二值图非零像素数 |
| `candidate_count` | 轮廓筛选统计 | 本帧通过形状/面积筛选的候选轮廓数 |
| `jpeg_bytes` | 实际编码结果 | 本帧图像字节数 |

> `lock_frames` / `lost_frames` / `green_pixels` / `candidate_count` 为 M1 新增的**只读统计接口**，
> 只是把 `GreenDetect` 已有的内部变量暴露出来，**不参与任何算法判定**。

## 4. 启动方式

```bash
# 方式 A：一键（Flask + 主程序，默认使用海康相机）
./run_web_debug.sh

# 方式 B：无工业相机联调（合成绿色引导灯视频源）
./run_web_debug.sh --source synthetic --no-serial --no-gui --no-record

# 方式 C：用测试图片作为视频源
./run_web_debug.sh --source image:test/test.png --no-serial --no-gui --no-record

# 方式 D：只启动 Flask，视觉程序另行启动
./run_web_debug.sh --backend-only
```

浏览器打开 <http://127.0.0.1:5000/>。

主程序新增参数（全部可选，不指定时与原行为一致）：

| 参数 | 作用 |
|---|---|
| `--source <spec>` | 使用模拟视频源：`synthetic` / `synthetic:none` / `image:<path>` / `video:<path>` |
| `--no-serial` | 不打开串口，使用模拟发送（无电控环境测试） |
| `--no-gui` | 不创建 OpenCV 窗口（无头运行） |
| `--no-record` | 不写 `record.avi` |
| `--web-debug` / `--no-web-debug` | 强制开/关调试数据发布（覆盖 YAML 配置） |

配置项（`config/debug.yaml`）：

```yaml
web_debug:
  enabled: true
  host: "127.0.0.1"
  port: 5010
  publish_fps: 20
  jpeg_quality: 75
```

## 5. 自检

```bash
# 视觉程序 + Flask 都在运行时
python3 test/test_web_debug_checks.py

# 只启动 Flask（无视觉程序）时，验证降级行为
python3 test/test_web_debug_checks.py --offline
```

## 6. 故障隔离行为（实测）

| 场景 | 行为 |
|---|---|
| Flask 未启动 | C++ 侧每秒退避重连一次；调试帧丢弃（`dropped` 增长）；视觉/串口主链路照常运行 |
| Flask 中途退出 | C++ 侧检测到断开，继续退避重连；主循环无感知 |
| Flask 重启 | C++ 侧自动重连，调试数据恢复 |
| 浏览器未连接 | Flask 照常接收；MJPEG/SSE 生成器无人消费，不影响视觉线程 |
| 浏览器断开 | Flask 生成器正常结束；C++ 侧完全无感知 |

## 7. M2 扩展点

Flask 侧已预留占位（`/api/meta` 中可见），M2 可按以下方向扩展而不改动本层协议：

- 电控遥测：扩展现有 JSON（新增 `board` 对象）即可，帧协议无需变更；
- 参数管理：`GET/POST /api/config`（写回 `config/debug.yaml`，需参数热更新接口配合）；
- 录制：Flask 侧把最新帧落盘，或 C++ 侧复用现有 `VideoWriter`；
- 历史回放：会话目录 + MJPEG 的静态帧接口（参考 Misaka A5 只读回放结构）。

## 8. 与 Misaka 调试工程的关系

- 参考了 Misaka A1 的**界面信息结构**（顶栏状态、画面区、状态面板、实时曲线、数据来源标识），
  但前端为本项目独立实现（HTML/CSS/原生 JS + Canvas，无第三方前端库、无外部 CDN）；
- **未复制** Misaka 的后端代码（其依赖 UMT/pybind/嵌入式 Python，与本项目架构不兼容）；
- Misaka 工程全程只读，未做任何修改。

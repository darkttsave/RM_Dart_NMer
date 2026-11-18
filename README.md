# 飞镖制导系统

## 概述

用于检测远处绿色圆形 LED 灯，并通过串口向电控发送转向和距离阶段指令。

## 数据包格式

```text
帧头(1) + turnFlag(4) + distanceFlag(4) + clearFlag(4) + CRC16(2) = 15字节
```

| 字段 | 值 | 含义 |
|---|---|---|
| turnFlag | 0.0 | 停止转向（已对准） |
| turnFlag | 1.0 | 左转 |
| turnFlag | 2.0 | 右转 |
| turnFlag | 3.0 | 视野内无目标 |
| distanceFlag | 1.0 | 距离远，加速 |
| distanceFlag | 2.0 | 距离中，减速 |
| distanceFlag | 3.0 | 距离近/已对准/无目标 |

## 文件结构

```text
26new飞镖/
├── main.cpp
├── CMakeLists.txt
├── build_and_run.sh
├── watchdog.sh
├── config/
│   └── debug.yaml
├── CameraDriver/
│   ├── HaiKang.h
│   └── HaiKang.cpp
├── Communication/
│   ├── VirtualSerial.h / .cpp
│   ├── ByteConverter.h / .cpp
│   ├── CRC16.hpp
│   └── CRC16.cpp
├── ImgProcessing/
│   ├── GreenDetect.h
│   └── GreenDetect.cpp
├── Tool/
│   ├── Utils.h
│   └── Utils.cpp
├── camera/
├── SDK/                  # 本地放置 MVS SDK，不随仓库分发
├── test/
│   └── test_green_detect.cpp
└── logs/                  # 运行时自动生成
```

> **公开版本说明**：本仓库不包含 `SDK/`（海康专有 SDK）、`test/test.png`（实拍测试图）、
> `CameraDriver/docs/相机启动流程.png`（第三方博客截图）、`build/`、`logs/`。
> 具体忽略规则见 `.gitignore`，SDK 获取方式见文末「依赖」。

## 编译和运行

普通模式：

```bash
chmod +x build_and_run.sh
./build_and_run.sh
```

看门狗模式（串口断开或发送失败时自动重启）：

```bash
chmod +x watchdog.sh build_and_run.sh
./watchdog.sh
```

> 看门狗行为由 `config/debug.yaml` 中的 `communication.watchdog` 控制：
> - `true`：串口打开失败或发送失败时程序退出，由 `watchdog.sh` 3 秒后自动重启
> - `false`：串口不可用时切换模拟模式，仅运行视觉算法

## 配置文件 `config/debug.yaml`

| 参数 | 说明 |
|---|---|
| camera.exposure | 曝光时间（微秒），可通过滑动条实时调整 |
| camera.gain | 增益，可通过滑动条实时调整 |
| camera.serial_number | 海康相机序列号 |
| detection.min_radius / max_radius | 绿灯半径范围（像素） |
| detection.bright_thresh | HSV V 通道亮度阈值（0~255） |
| detection.green_weight_thresh | 归一化绿色权重阈值 G/(R+G+B+1)（0.0~1.0） |
| detection.morphology.kernel_size | 形态学核大小 |
| detection.morphology.open_iterations | 开运算迭代次数 |
| detection.morphology.close_iterations | 闭运算迭代次数 |
| detection.shape_filter.min_circularity | 最小圆形度（0~1），建议 0.5~0.7 |
| detection.shape_filter.min_aspect_ratio | 外接矩形宽高比下限 |
| detection.shape_filter.max_aspect_ratio | 外接矩形宽高比上限 |
| detection.shape_filter.min_area | 轮廓最小面积（像素²） |
| detection.shape_filter.max_area | 轮廓最大面积（像素²） |
| detection.time_filter.required_frames | 进入追踪所需连续检测帧数 |
| detection.time_filter.required_lost_frames | 退出追踪所需连续丢失帧数 |
| detection.time_filter.max_center_dist | 相邻帧中心最大偏移（像素），超出则重置计数 |
| turn_control.turn_threshold | 转向死区（像素），小于此值输出 turnFlag=0 |
| turn_control.dist_far_thresh | 偏移大于该值时 distanceFlag=1（加速） |
| turn_control.dist_near_thresh | 偏移小于该值时 distanceFlag=3（对准） |
| communication.port | 串口设备路径 |
| communication.watchdog | 见上方看门狗说明 |
| debug_setting.show_tx_terminal | 是否在终端打印每次串口发送内容 |
| runtime_logging.terminal_line_limit | 启动阶段保存到日志的终端行数 |

## 运行时窗口

| 窗口名 | 内容 |
|---|---|
| 飞镖制导系统（处理用） | 原始分辨率缩小至 1/2 的调试图像 |
| 飞镖制导系统（结果展示-超大） | 原始分辨率放大至 3x 的调试图像，同时录入视频 |
| Binary Image (BGR) | 绿色二值化掩码图 |
| BGR Control | 滑动条面板，实时调整所有检测参数 |

## 调试图像说明

`debugImage_` 左上角实时显示状态（两行，互不遮挡）：

| 情况 | 第一行 | 第二行 |
|---|---|---|
| 无绿色像素 | `No Green In View`（红） | — |
| 有绿色但轮廓被过滤 | `No Target  lock:0/N`（红） | — |
| 追踪中暂时丢失 | `Tracking  lost:X/N`（红） | — |
| 积累锁定中 | `Lock: X/N`（绿） | — |
| 已进入追踪 | `Tracking  lost:0/N`（绿） | `Turn: LEFT / RIGHT / ALIGN`（黄） |

图像上每个面积 > 10px² 的绿色轮廓都会叠加外接矩形和参数标注：

- **绿框**：全部筛选条件通过；**红框**：至少一个条件不满足
- `A` 面积（对应 min_area / max_area）
- `R` 半径（对应 min_radius / max_radius）
- `C` 圆形度（对应 min_circularity）
- `W` 宽高比（对应 min/max_aspect_ratio）
- 各参数标注颜色与对应条件是否通过一致

## 运行日志

- 每次启动在项目根目录创建 `logs/<yyMMddHHmm>/`
- `startup_terminal.log`：启动阶段前 N 行终端输出（N 由 `terminal_line_limit` 控制）
- `record.avi`：运行期间超大窗口的视频录像（MJPG 编码，30fps）

## 识别流程

1. 亮区掩码：HSV V 通道 > `bright_thresh`
2. 绿色权重掩码：G/(R+G+B+1) > `green_weight_thresh`
3. 两者取交集，形态学开/闭运算去噪
4. 轮廓提取，依次按面积、圆形度、宽高比过滤
5. 最大合格轮廓用图像矩计算质心，`minEnclosingCircle` 获取半径
6. 时间滤波：连续 `required_frames` 帧稳定后进入追踪；连续 `required_lost_frames` 帧丢失后退出追踪
7. 追踪状态下计算转向方向和距离阶段，通过串口发送

## 依赖

- OpenCV（4.x）
- yaml-cpp（0.7 / 0.8）
- 海康威视 MVS SDK（相机采集必需，**不随本仓库分发**）

### 相机 SDK（MVS）获取与放置

本仓库不包含海康威视 MVS SDK（专有软件，避免再分发）。使用前请自行获取：

1. 访问海康机器人官网下载 **MVS**（Linux 64 位版本）；
2. 将头文件与库放入项目的 `SDK/` 目录：

```bash
mkdir -p SDK/include SDK/lib
cp MVS/Development/include/*.h SDK/include/
cp MVS/Development/lib/64/libMvCameraControl.so SDK/lib/
```

3. `CMakeLists.txt` 会自动在 `SDK/include` 与 `SDK/lib` 下查找；若已安装到系统路径（`/usr/local`、`/opt/MVS` 等），也可不复制。

## 第三方组件与许可

第三方代码的原始版权声明均**保留未删**：

| 组件 | 位置 | 保留的版权声明 | 许可证情况 |
|---|---|---|---|
| RMVL 相机库（裁剪快照） | `camera/` | RoboMaster Vision Community (2021/2023)、RMVL Community (2024)、zhaoxi (2023) | 仓库内**未附**许可证全文（见下方说明） |
| CRC16 校验 | `Communication/CRC16.*` | Copyright (c) 2022 ChenJun | Apache-2.0（源码头声明） |
| 海康威视 MVS SDK | `SDK/`（不随仓库分发） | 海康机器人 | 专有软件 |

> **说明**：`camera/` 为 RMVL 社区相机库的裁剪快照，文件头保留了原作者与版权年份；仓库内未附带该组件的许可证全文，对外分发前请与上游项目确认。**本项目未自行选择或变更项目整体的开源许可证。**

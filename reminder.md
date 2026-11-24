# 启动提醒

在项目根目录执行，首次运行或修改代码后先构建：

```bash
cmake -S . -B build && cmake --build build -j2
```

```bash
./run_web_debug.sh                                         # 相机、串口与实时预览
./run_web_debug.sh --source synthetic --no-serial --no-gui # 模拟源，无需相机和电控
./run_web_debug.sh --record-only                           # 无页面自动录制
./run_web_debug.sh --record-only --source synthetic --no-serial # 模拟录制
./run_web_debug.sh --replay                                # 历史回放
```

实时页面：<http://127.0.0.1:5000/>；回放：<http://127.0.0.1:5001/>。按 `Ctrl+C` 退出。
配置文件：`config/debug.yaml`；更多选项：`./run_web_debug.sh --help`。

相机单独测试（读取 `config/debug.yaml`，默认采集 30 帧）：

```bash
./build/camera_test                 # 无界面采集并输出帧率
./build/camera_test --display --frames=0 # 持续预览，按 q 或 Esc 退出
./run_web_debug.sh --headless --no-serial --no-record --perf # 相机及处理耗时统计
```

独立运行（无 Flask 和 OpenCV 窗口）：

```bash
./run_web_debug.sh --headless --source synthetic --no-serial
./run_web_debug.sh --headless --source synthetic --no-serial --auto-record --record-image-mode raw
```

Web 的「参数调整」包含飞镖参数，状态面板可重置飞镖跟踪。自动录像的默认来源由 `recording.record_image_mode` 配置。

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
```

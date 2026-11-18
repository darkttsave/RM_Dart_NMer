#!/bin/bash
# 飞镖制导系统编译和运行脚本

echo "飞镖制导系统编译和运行脚本"
echo "===================="

cd "$(dirname "$0")"

# 检查是否存在build目录
if [ ! -d "build" ]; then
    echo "创建build目录..."
    mkdir build
fi

# 进入build目录
cd build

# 检查是否需要重新编译（通过比较可执行文件的修改时间）
need_build=true
if [ -f "main" ]; then
    # 如果有可执行文件，检查源文件是否更新
    if [ "../CMakeLists.txt" -ot "main" ] && \
       [ "../main.cpp" -ot "main" ] && \
       [ -d "../ImgProcessing" ] && \
       [ -d "../Communication" ]; then
        need_build=false
        echo "可执行文件是最新的，跳过编译"
    fi
fi

# 如果需要编译，执行编译
if [ "$need_build" = true ]; then
    echo "需要重新编译..."
    
    # 运行cmake配置
    echo "运行CMake配置..."
    cmake .. > /dev/null 2>&1
    
    # 编译项目
    echo "编译项目..."
    make -j4
    
    # 检查编译结果
    if [ $? -ne 0 ]; then
        echo "编译失败！"
        exit 1
    fi
    
    echo "编译成功！"
else
    echo "使用现有可执行文件"
fi

# 检查可执行文件是否存在
if [ ! -f "main" ]; then
    echo "错误：可执行文件不存在"
    exit 1
fi

# 检查串口设备是否存在
if [ ! -e "/dev/ttyACM0" ]; then
    echo "警告：串口设备 /dev/ttyACM0 不存在"
    echo "程序将以测试模式运行"
fi

# 运行程序
echo ""
echo "启动飞镖制导系统..."
echo ""
./main

echo ""
echo "程序退出"


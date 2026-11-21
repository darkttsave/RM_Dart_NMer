// 独立相机采集检查，不依赖串口、识别或 Web 调试服务。
#include "io/camera/HaiKang.h"

#include <chrono>
#include <csignal>
#include <iostream>
#include <opencv2/opencv.hpp>

namespace {
volatile std::sig_atomic_t stopRequested = 0;
void stop(int) { stopRequested = 1; }
}

int main(int argc, char **argv)
{
    const cv::String keys =
        "{help h | | 显示帮助 }"
        "{display d | | 显示相机画面，按 q 或 Esc 退出 }"
        "{frames | 30 | 成功采集的帧数，0 表示持续采集 }";
    cv::CommandLineParser cli(argc, argv, keys);
    cli.about("海康相机启动测试（读取 config/debug.yaml）");
    if (cli.has("help")) { cli.printMessage(); return 0; }
    const int frameLimit = cli.get<int>("frames");
    if (!cli.check() || frameLimit < 0)
    {
        cli.printErrors();
        std::cerr << "[camera_test] --frames 必须是非负整数" << std::endl;
        return 2;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);

    Camera_ camera;
    if (!camera.isOpened())
    {
        std::cerr << "[camera_test] 无法打开相机，请检查连接、SDK 和 config/debug.yaml 的序列号。" << std::endl;
        return 1;
    }
    const bool display = cli.has("display");
    const auto start = std::chrono::steady_clock::now();
    int frames = 0, emptyFrames = 0;
    while (!stopRequested && (frameLimit == 0 || frames < frameLimit))
    {
        cv::Mat image;
        camera.getimage(image);
        if (image.empty())
        {
            if (++emptyFrames >= 3)
            {
                std::cerr << "[camera_test] 连续三次未获取到图像。" << std::endl;
                return 1;
            }
            continue;
        }
        emptyFrames = 0;
        if (++frames == 1)
            std::cout << "[camera_test] 图像尺寸: " << image.cols << "x" << image.rows << std::endl;
        if (display)
        {
            cv::imshow("Camera test", image);
            const int key = cv::waitKey(1);
            if (key == 'q' || key == 27) break;
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "[camera_test] 采集 " << frames << " 帧，平均 "
              << (seconds > 0 ? frames / seconds : 0) << " FPS" << std::endl;
    return frames > 0 ? 0 : 1;
}

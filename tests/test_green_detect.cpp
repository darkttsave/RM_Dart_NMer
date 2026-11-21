/**
 * @file test_green_detect.cpp
 * @brief 离线模式：用测试图片模拟相机帧流，验证曝光/增益和BGR参数
 *
 * 用法: ./test_green_detect <图像路径> [图像路径2 ...]
 * 曝光/增益滑动条模拟相机参数对图像亮度的影响
 */

#include <iostream>
#include <vector>
#include <string>
#include <opencv2/opencv.hpp>
#include "tasks/dart/GreenDetect.h"

using namespace cv;
using namespace std;

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        cerr << "用法: " << argv[0] << " <图像路径> [图像路径2 ...]" << endl;
        return -1;
    }

    vector<Mat> frames;
    for (int i = 1; i < argc; ++i)
    {
        Mat img = imread(argv[i]);
        if (img.empty())
        {
            cerr << "无法读取: " << argv[i] << endl;
        }
        else
        {
            frames.push_back(img);
            cout << "已加载: " << argv[i] << " (" << img.cols << "x" << img.rows << ")" << endl;
        }
    }
    if (frames.empty())
    {
        cerr << "没有可用图像" << endl;
        return -1;
    }

    GreenDetect detector;

    namedWindow("Controls", WINDOW_AUTOSIZE);
    Mat panel = Mat::zeros(1, 512, CV_8UC3);
    imshow("Controls", panel);

    // 相机参数模拟（对应 debug.yaml 默认值）
    int exposure = 10000; // 对应 config: exposure: 800
    int gain = 10;      // 对应 config: gain: 10

    int brightThresh = 80;
    int greenWeightX100 = 40;
    // 形状过滤参数（x100 存储，实际值除以100）
    int circularityX100 = 60;   // min_circularity * 100
    int minAspX100 = 50;        // min_aspect_ratio * 100
    int maxAspX100 = 200;       // max_aspect_ratio * 100

    createTrackbar("Exposure(x10us)", "Controls", &exposure, 5000);
    createTrackbar("Gain", "Controls", &gain, 100);
    createTrackbar("BrightThresh", "Controls", &brightThresh, 255);
    createTrackbar("GreenWeight x100", "Controls", &greenWeightX100, 100);
    createTrackbar("Circ x100", "Controls", &circularityX100, 100);
    createTrackbar("AspRatio_min x100", "Controls", &minAspX100, 300);
    createTrackbar("AspRatio_max x100", "Controls", &maxAspX100, 500);

    cout << "\n按键: ESC=退出  空格=下一张\n"<< endl;

    int frameIdx = 0;
    while (true)
    {
        const Mat &raw = frames[frameIdx % frames.size()];

        // 用 alpha/beta 模拟曝光+增益对亮度的影响
        // alpha: 增益倍率 (gain/10 映射到 0.5~3.0)
        // beta:  曝光偏置 (exposure 越大图越亮)
        double alpha = 0.5 + gain / 20.0;
        double beta = (exposure - 800) / 50.0; // 以800为基准，偏移量
        Mat simulated;
        raw.convertTo(simulated, -1, alpha, beta);

        detector.setBrightThresh(brightThresh);
        detector.setGreenWeightThresh(greenWeightX100 / 100.0f);
        detector.setShapeFilter(circularityX100 / 100.0, minAspX100 / 100.0, maxAspX100 / 100.0);

        auto target = detector.detectGreenTarget(simulated);

        Mat binary = detector.getBinaryImage();
        if (!binary.empty())
            imshow("Binary", binary);

        Mat dbg = detector.getDebugImage();
        if (!dbg.empty())
        {
            string info = target.detected
                              ? "Detected (" + to_string((int)target.center.x) + "," + to_string((int)target.center.y) +
                                    ") r=" + to_string((int)target.radius)
                              : "Not Detected";
            putText(dbg, info,
                    Point(10, 30), FONT_HERSHEY_SIMPLEX, 0.7, Scalar(0, 255, 255), 2);
            string camInfo = "Exp=" + to_string(exposure) + " Gain=" + to_string(gain) +
                             "  [" + to_string(frameIdx % (int)frames.size() + 1) +
                             "/" + to_string(frames.size()) + "]";
            putText(dbg, camInfo,
                    Point(10, 60), FONT_HERSHEY_SIMPLEX, 0.55, Scalar(255, 200, 0), 1);
            imshow("Detection Result", dbg);
        }

        imshow("Simulated Frame", simulated);

        int key = waitKey(30);
        if (key == 27)
            break;
        if (key == 32)
        {
            ++frameIdx;
            cout << "切换到第 " << (frameIdx % frames.size() + 1) << " 张" << endl;
        }
    }

    destroyAllWindows();
    return 0;
}

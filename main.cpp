/**
 * @file main.cpp
 * @brief 飞镖制导系统主程序
 * @author lxy
 * @date 2025-10-24
 */

#include <iostream>
#include <vector>
#include <opencv2/highgui.hpp>
#include <opencv2/opencv.hpp>
#include <yaml-cpp/yaml.h>
#include "CameraDriver/HaiKang.h"
#include "Communication/VirtualSerial.h"
#include "ImgProcessing/GreenDetect.h"
#include "Tool/Utils.h"

using namespace cv;
using namespace std;

int main()
{
    // 调试配置: true 时持续发送测试值，false 时根据视觉结果发送控制指令
    bool testCommMode = false;
    float testCommValue = 1.0f;

    // 0. 从 YAML 配置文件加载参数
    double turnThreshold = 5.0;
    double distFarThresh = 200.0;
    double distNearThresh = 30.0;
    std::string serialPort = "/dev/ttyACM0";
    bool showTxTerminal = false;
    bool serialWatchdog = false;
    int terminalLogLineLimit = 50;
    int brightThresh = 80;
    int greenWeightX100 = 40; // green_weight_thresh * 100
    int exposure = 3200, gain = 10;
    int circularityX100 = 60;
    int minAspX100 = 50, maxAspX100 = 200;
    int minArea = 50, maxArea = 50000;
    int minRadius = 10, maxRadius = 30;
    std::string loadedConfigPath;

    try
    {
        const std::vector<std::string> configCandidates = {
            "../config/debug.yaml",
            "config/debug.yaml",
            "./config/debug.yaml"};

        YAML::Node config;
        bool loaded = false;
        for (const auto &path : configCandidates)
        {
            try
            {
                config = YAML::LoadFile(path);
                loadedConfigPath = path;
                loaded = true;
                break;
            }
            catch (const std::exception &)
            {
                // 尝试下一个候选路径
            }
        }

        if (!loaded)
        {
            throw std::runtime_error("配置文件加载失败");
        }

        if (config["turn_control"] && config["turn_control"]["turn_threshold"])
            turnThreshold = config["turn_control"]["turn_threshold"].as<double>();
        if (config["turn_control"] && config["turn_control"]["dist_far_thresh"])
            distFarThresh = config["turn_control"]["dist_far_thresh"].as<double>();
        if (config["turn_control"] && config["turn_control"]["dist_near_thresh"])
            distNearThresh = config["turn_control"]["dist_near_thresh"].as<double>();
        if (config["communication"] && config["communication"]["port"])
            serialPort = config["communication"]["port"].as<std::string>();
        if (config["communication"] && config["communication"]["watchdog"])
            serialWatchdog = config["communication"]["watchdog"].as<bool>();
        if (config["debug_setting"] && config["debug_setting"]["show_tx_terminal"])
            showTxTerminal = config["debug_setting"]["show_tx_terminal"].as<bool>();
        if (config["runtime_logging"] && config["runtime_logging"]["terminal_line_limit"])
            terminalLogLineLimit = config["runtime_logging"]["terminal_line_limit"].as<int>();
        if (config["detection"])
        {
            if (config["detection"]["bright_thresh"])
                brightThresh = config["detection"]["bright_thresh"].as<int>();
            if (config["detection"]["green_weight_thresh"])
                greenWeightX100 = (int)(config["detection"]["green_weight_thresh"].as<float>() * 100);
            if (config["detection"]["min_radius"])
                minRadius = config["detection"]["min_radius"].as<int>();
            if (config["detection"]["max_radius"])
                maxRadius = config["detection"]["max_radius"].as<int>();
        }
        if (config["camera"])
        {
            if (config["camera"]["exposure"])
                exposure = config["camera"]["exposure"].as<int>();
            if (config["camera"]["gain"])
                gain = config["camera"]["gain"].as<int>();
        }
        if (config["detection"] && config["detection"]["shape_filter"])
        {
            auto sf = config["detection"]["shape_filter"];
            if (sf["min_circularity"])
                circularityX100 = (int)(sf["min_circularity"].as<double>() * 100);
            if (sf["min_aspect_ratio"])
                minAspX100 = (int)(sf["min_aspect_ratio"].as<double>() * 100);
            if (sf["max_aspect_ratio"])
                maxAspX100 = (int)(sf["max_aspect_ratio"].as<double>() * 100);
            if (sf["min_area"])
                minArea = sf["min_area"].as<int>();
            if (sf["max_area"])
                maxArea = sf["max_area"].as<int>();
        }
    }
    catch (const std::exception &e)
    {
        cout << "[Main] Warning: 无法加载配置文件，使用默认值。原因: " << e.what() << endl;
    }

    if (terminalLogLineLimit < 0)
    {
        terminalLogLineLimit = 0;
    }

    const std::string runTimestamp = makeRunTimestamp();
    const auto projectRootDir = resolveProjectRoot(loadedConfigPath);
    const auto logRootDir = projectRootDir / "logs";
    const auto runLogDir = logRootDir / runTimestamp;
    std::filesystem::create_directories(runLogDir);

    setupTerminalLogCapture(runLogDir / "startup_terminal.log", terminalLogLineLimit);

    cout << "[RunLog] Project root directory: " << projectRootDir << endl;
    cout << "[RunLog] Current run directory: " << runLogDir << endl;
    cout << "[RunLog] Startup terminal line limit: " << terminalLogLineLimit << endl;
    cout << "[Main] Config path: " << loadedConfigPath << endl;
    cout << "[Main] Turn threshold: " << turnThreshold << endl;
    cout << "[Main] Serial port: " << serialPort << endl;
    cout << "[Main] show_tx_terminal: " << (showTxTerminal ? "true" : "false") << endl;
    cout << "[Main] serial_watchdog: " << (serialWatchdog ? "true" : "false") << endl;

    // 1. 初始化相机
    Camera_ camera;

    // 2. 初始化绿灯识别
    GreenDetect detector;
    detector.setTurnThreshold(turnThreshold);
    detector.setDistanceThresholds(distFarThresh, distNearThresh);

    // 3. 初始化串口通信
    VirtualSerial serial(serialPort);
    serial.SetTxLogEnabled(showTxTerminal);
    if (!serial.Open())
    {
        if (serialWatchdog)
        {
            cerr << "[Main] Error: Failed to open serial port, exiting for watchdog restart." << endl;
            return 1;
        }
        cerr << "[Main] Warning: Failed to open serial port (" << serialPort << ")" << endl;
        cerr << "[Main] Info: 切换到无串口模拟模式，仅运行视觉算法，发送逻辑将被跳过。" << endl;
        serial.SetSimulated(true);
    }

    Mat controlPanel = Mat::zeros(300, 512, CV_8UC3);
    namedWindow("BGR Control", cv::WINDOW_AUTOSIZE);
    imshow("BGR Control", controlPanel);

    createTrackbar("Bright(0-255)", "BGR Control", &brightThresh, 255);
    createTrackbar("Green% x100", "BGR Control", &greenWeightX100, 100);
    createTrackbar("Circ% x100", "BGR Control", &circularityX100, 100);
    createTrackbar("Asp_min x100", "BGR Control", &minAspX100, 300);
    createTrackbar("Asp_max x100", "BGR Control", &maxAspX100, 500);
    createTrackbar("Area_min(px2)", "BGR Control", &minArea, 10000);
    createTrackbar("Area_max(px2)", "BGR Control", &maxArea, 100000);
    createTrackbar("Radius_min(px)", "BGR Control", &minRadius, 200);
    createTrackbar("Radius_max(px)", "BGR Control", &maxRadius, 200);
    createTrackbar("Exposure(us)", "BGR Control", &exposure, 100000);
    createTrackbar("Gain", "BGR Control", &gain, 100);

    // 4. 主循环
    Mat image;
    Mat resizedImage;
    Mat displayImage;
    const int DISPLAY_SCALE = 2;
    const int RESULT_SCALE = 3;

    Size originalSize;
    Size displaySize;
    Size resultSize;
    cv::VideoWriter videoWriter;

    while (true)
    {
        if (serialWatchdog && !serial.IsSimulated() && !serial.IsOpen())
        {
            cerr << "[Main] Serial port disconnected, exiting for watchdog restart." << endl;
            return 1;
        }

        camera.setCameraExposureGain(exposure, gain);
        camera.setCameraExposureGain(exposure, gain);
        camera.getimage(image);
        if (image.empty())
        {
            continue;
        }

        if (originalSize.width == 0)
        {
            originalSize = image.size();
            displaySize = Size(originalSize.width / DISPLAY_SCALE, originalSize.height / DISPLAY_SCALE);
            resultSize = Size(originalSize.width * RESULT_SCALE, originalSize.height * RESULT_SCALE);
            videoWriter.open((runLogDir / "record.avi").string(),
                             cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 30, resultSize);
        }

        detector.setBrightThresh(brightThresh);
        detector.setGreenWeightThresh(greenWeightX100 / 100.0f);
        detector.setShapeFilter(circularityX100 / 100.0, minAspX100 / 100.0, maxAspX100 / 100.0);
        detector.setAreaFilter(minArea, maxArea);
        detector.setRadiusFilter(minRadius, maxRadius);

        auto target = detector.detectGreenTarget(image);

        Mat binaryImage = detector.getBinaryImage();
        if (!binaryImage.empty())
        {
            imshow("Binary Image (BGR)", binaryImage);
        }

        if (testCommMode)
        {
            if (!serial.sendCommand(testCommValue) && serialWatchdog)
                return 1;
        }
        else
        {
            if (target.detected)
            {
                if (!serial.sendCommand(target.turnFlag, target.distanceFlag) && serialWatchdog)
                    return 1;
            }
            else if (target.distanceFlag == 3.0f)
            {
                if (!serial.sendCommand(3.0f, 3.0f) && serialWatchdog)
                    return 1;
            }
        }

        if (target.detected)
        {
            Mat debugImage = detector.getDebugImage();
            resize(debugImage, resizedImage, displaySize);
            resize(debugImage, displayImage, resultSize, 0, 0, INTER_NEAREST);

            imshow("飞镖制导系统（处理用）", resizedImage);
            imshow("飞镖制导系统（结果展示-超大）", displayImage);
        }
        else
        {
            Mat debugImage = detector.getDebugImage();
            if (debugImage.empty())
                debugImage = image;
            resize(debugImage, resizedImage, displaySize);
            resize(debugImage, displayImage, resultSize, 0, 0, INTER_NEAREST);

            imshow("飞镖制导系统（处理用）", resizedImage);
            imshow("飞镖制导系统（结果展示-超大）", displayImage);
        }

        if (videoWriter.isOpened() && !displayImage.empty())
        {
            videoWriter.write(displayImage);
        }

        if (waitKey(1) == 27)
        {
            break;
        }
    }

    serial.Close();
    destroyAllWindows();

    restoreTerminalOutput();

    return 0;
}

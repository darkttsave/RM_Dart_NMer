/**
 * @file main.cpp
 * @brief 飞镖制导系统主程序
 * @author lxy
 * @date 2025-10-24
 */

#include <iostream>
#include <vector>
#include <chrono>
#include <memory>
#include <thread>
#include <opencv2/highgui.hpp>
#include <opencv2/opencv.hpp>
#include <yaml-cpp/yaml.h>
#include "CameraDriver/HaiKang.h"
#include "Communication/VirtualSerial.h"
#include "ImgProcessing/GreenDetect.h"
#include "Tool/Utils.h"
#include "web_debug/DebugTelemetry.h"
#include "web_debug/FrameSource.h"

using namespace cv;
using namespace std;

namespace
{
/**
 * @brief M1 调试链路命令行参数
 *
 * 全部为可选参数，不指定时行为与原有主程序完全一致（海康相机 + 串口 + GUI）。
 */
struct CliOptions
{
    std::string sourceSpec;        // 空 = 海康工业相机；synthetic / image:<path> / video:<path>
    bool forceSimSerial = false;   // --no-serial: 跳过串口打开，仅模拟发送
    bool showGui = true;           // --no-gui: 不创建 OpenCV 窗口（无头测试/服务器）
    bool recordVideo = true;       // --no-record: 不写 record.avi（无头测试时避免磁盘占用）
    int webDebugOverride = -1;     // -1=跟随配置文件, 0=强制关闭, 1=强制开启
};

void printUsage(const char *prog)
{
    cout << "飞镖制导系统（含 M1 Web 调试链路）\n"
         << "用法: " << prog << " [选项]\n"
         << "  --source <spec>     使用模拟视频源（默认使用海康工业相机）\n"
         << "                      spec: synthetic | synthetic:none | image:<path> | video:<path>\n"
         << "  --no-serial         不打开串口，使用模拟发送（无电控环境测试）\n"
         << "  --no-gui            不创建 OpenCV 显示窗口（无头运行）\n"
         << "  --no-record         不写 record.avi（无头测试时避免磁盘占用）\n"
         << "  --web-debug         强制启用 Web 调试数据发布（覆盖配置）\n"
         << "  --no-web-debug      强制关闭 Web 调试数据发布（覆盖配置）\n"
         << "  -h, --help          显示帮助\n";
}

CliOptions parseCli(int argc, char **argv)
{
    CliOptions opt;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--source" && i + 1 < argc)
            opt.sourceSpec = argv[++i];
        else if (arg == "--no-serial")
            opt.forceSimSerial = true;
        else if (arg == "--no-gui")
            opt.showGui = false;
        else if (arg == "--no-record")
            opt.recordVideo = false;
        else if (arg == "--web-debug")
            opt.webDebugOverride = 1;
        else if (arg == "--no-web-debug")
            opt.webDebugOverride = 0;
        else if (arg == "-h" || arg == "--help")
        {
            printUsage(argv[0]);
            std::exit(0);
        }
        else
            cerr << "[Main] Warning: 未知参数: " << arg << endl;
    }
    return opt;
}
} // namespace

int main(int argc, char **argv)
{
    const CliOptions cli = parseCli(argc, argv);

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

    // Web 调试数据发布配置（config/debug.yaml 的 web_debug 段，可被命令行覆盖）
    bool webDebugEnabled = true;
    std::string webDebugHost = "127.0.0.1";
    int webDebugPort = 5010;
    int webDebugPublishFps = 20;
    int webDebugJpegQuality = 75;
    std::string cameraSerialNumber = "unknown";

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
            if (config["camera"]["serial_number"])
                cameraSerialNumber = config["camera"]["serial_number"].as<std::string>();
        }
        // M1：Web 调试数据发布配置（可选段，缺省时使用内置默认值）
        if (config["web_debug"])
        {
            auto wd = config["web_debug"];
            if (wd["enabled"])
                webDebugEnabled = wd["enabled"].as<bool>();
            if (wd["host"])
                webDebugHost = wd["host"].as<std::string>();
            if (wd["port"])
                webDebugPort = wd["port"].as<int>();
            if (wd["publish_fps"])
                webDebugPublishFps = wd["publish_fps"].as<int>();
            if (wd["jpeg_quality"])
                webDebugJpegQuality = wd["jpeg_quality"].as<int>();
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

    // 任何退出路径（含 watchdog 提前 return）都恢复终端输出。
    // 说明：原代码在提前 return 时未恢复 tee 流，进程退出阶段 std::cout 仍指向
    // 已被析构的日志缓冲区，可能触发段错误（M1 联调时复现）。此处用 RAII 统一修复。
    struct TerminalOutputGuard
    {
        ~TerminalOutputGuard() { restoreTerminalOutput(); }
    } terminalOutputGuard;
    (void)terminalOutputGuard;

    cout << "[RunLog] Project root directory: " << projectRootDir << endl;
    cout << "[RunLog] Current run directory: " << runLogDir << endl;
    cout << "[RunLog] Startup terminal line limit: " << terminalLogLineLimit << endl;
    cout << "[Main] Config path: " << loadedConfigPath << endl;
    cout << "[Main] Turn threshold: " << turnThreshold << endl;
    cout << "[Main] Serial port: " << serialPort << endl;
    cout << "[Main] show_tx_terminal: " << (showTxTerminal ? "true" : "false") << endl;
    cout << "[Main] serial_watchdog: " << (serialWatchdog ? "true" : "false") << endl;

    // 命令行覆盖调试发布开关（不指定则跟随 config/debug.yaml）
    if (cli.webDebugOverride >= 0)
        webDebugEnabled = (cli.webDebugOverride == 1);

    // 1. 初始化相机（默认）或模拟视频源（--source，仅用于无相机联调）
    std::unique_ptr<Camera_> camera;
    std::unique_ptr<webdebug::FrameSource> frameSource;
    if (cli.sourceSpec.empty())
    {
        camera = std::make_unique<Camera_>();
    }
    else
    {
        std::string frameSourceError;
        frameSource = webdebug::createFrameSource(cli.sourceSpec, &frameSourceError);
        if (!frameSource)
        {
            cerr << "[Main] Error: 模拟视频源初始化失败: " << frameSourceError << endl;
            return 2;
        }
        cout << "[Main] 模拟视频源: " << frameSource->sourceLabel() << endl;
    }

    // 2. 初始化绿灯识别
    GreenDetect detector;
    detector.setTurnThreshold(turnThreshold);
    detector.setDistanceThresholds(distFarThresh, distNearThresh);

    // 3. 初始化串口通信
    VirtualSerial serial(serialPort);
    serial.SetTxLogEnabled(showTxTerminal);
    if (cli.forceSimSerial)
    {
        cout << "[Main] --no-serial: 跳过串口打开，使用模拟发送（仅调试，不产生真实控制指令）" << endl;
        serial.SetSimulated(true);
    }
    else if (!serial.Open())
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

    if (cli.showGui)
    {
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
    }

    // 4. M1 调试数据发布（独立后台线程：视觉线程只做非阻塞拷贝，故障不影响主链路）
    webdebug::Options webOptions;
    webOptions.enabled = webDebugEnabled;
    webOptions.host = webDebugHost;
    webOptions.port = webDebugPort;
    webOptions.publish_fps = webDebugPublishFps;
    webOptions.jpeg_quality = webDebugJpegQuality;
    webOptions.mode = "run";
    if (camera)
    {
        webOptions.source = "hikvision";
        webOptions.source_label = "海康工业相机 " + cameraSerialNumber;
    }
    else
    {
        webOptions.source = frameSource->sourceKind();
        webOptions.source_label = frameSource->sourceLabel();
    }
    webdebug::DebugTelemetry telemetry;
    telemetry.configure(webOptions);
    telemetry.start();

    // 5. 主循环
    Mat image;
    Mat resizedImage;
    Mat displayImage;
    const int DISPLAY_SCALE = 2;
    const int RESULT_SCALE = 3;

    Size originalSize;
    Size displaySize;
    Size resultSize;
    cv::VideoWriter videoWriter;

    auto lastFrameTime = std::chrono::steady_clock::now();
    double fpsEma = 0.0;

    while (true)
    {
        if (serialWatchdog && !serial.IsSimulated() && !serial.IsOpen())
        {
            cerr << "[Main] Serial port disconnected, exiting for watchdog restart." << endl;
            return 1;
        }

        if (camera)
        {
            camera->setCameraExposureGain(exposure, gain);
            camera->setCameraExposureGain(exposure, gain);
            camera->getimage(image);
        }
        else
        {
            if (!frameSource->next(image))
            {
                if (cli.showGui)
                    waitKey(1);
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
        }
        if (image.empty())
        {
            continue;
        }

        if (originalSize.width == 0)
        {
            originalSize = image.size();
            displaySize = Size(originalSize.width / DISPLAY_SCALE, originalSize.height / DISPLAY_SCALE);
            resultSize = Size(originalSize.width * RESULT_SCALE, originalSize.height * RESULT_SCALE);
            if (cli.recordVideo)
                videoWriter.open((runLogDir / "record.avi").string(),
                                 cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 30, resultSize);
        }

        detector.setBrightThresh(brightThresh);
        detector.setGreenWeightThresh(greenWeightX100 / 100.0f);
        detector.setShapeFilter(circularityX100 / 100.0, minAspX100 / 100.0, maxAspX100 / 100.0);
        detector.setAreaFilter(minArea, maxArea);
        detector.setRadiusFilter(minRadius, maxRadius);

        // 主循环帧率（含采集/显示开销）与识别处理耗时：来自实际运行，非估算
        const auto frameStartTime = std::chrono::steady_clock::now();
        auto target = detector.detectGreenTarget(image);
        const auto detectEndTime = std::chrono::steady_clock::now();
        const double processingTimeMs =
            std::chrono::duration<double, std::milli>(detectEndTime - frameStartTime).count();
        {
            const double dt = std::chrono::duration<double>(frameStartTime - lastFrameTime).count();
            lastFrameTime = frameStartTime;
            if (dt > 1e-6)
            {
                const double instantFps = 1.0 / dt;
                fpsEma = (fpsEma <= 0.0) ? instantFps : fpsEma * 0.9 + instantFps * 0.1;
            }
        }

        if (cli.showGui)
        {
            Mat binaryImage = detector.getBinaryImage();
            if (!binaryImage.empty())
            {
                imshow("Binary Image (BGR)", binaryImage);
            }
        }

        // 串口发送（与原有逻辑完全一致，仅额外记录本帧实际发送值用于调试展示）
        bool txSent = false;
        float txTurn = 0.0f;
        float txDist = 0.0f;
        float txClear = 1.0f;
        if (testCommMode)
        {
            txTurn = testCommValue;
            txDist = 0.0f;
            txClear = 1.0f;
            const bool ok = serial.sendCommand(testCommValue);
            txSent = true;
            if (!ok && serialWatchdog)
                return 1;
        }
        else
        {
            if (target.detected)
            {
                txTurn = target.turnFlag;
                txDist = target.distanceFlag;
                txClear = 1.0f;
                const bool ok = serial.sendCommand(target.turnFlag, target.distanceFlag);
                txSent = true;
                if (!ok && serialWatchdog)
                    return 1;
            }
            else if (target.distanceFlag == 3.0f)
            {
                txTurn = 3.0f;
                txDist = 3.0f;
                txClear = 1.0f;
                const bool ok = serial.sendCommand(3.0f, 3.0f);
                txSent = true;
                if (!ok && serialWatchdog)
                    return 1;
            }
        }

        // M1：发布本帧调试数据（非阻塞；Flask/浏览器未连接时后台线程直接丢弃）
        {
            webdebug::DebugFrame debugFrame;
            debugFrame.image = detector.getDebugImage();
            debugFrame.detected = target.detected;
            debugFrame.target_x = target.center.x;
            debugFrame.target_y = target.center.y;
            debugFrame.radius = target.radius;
            debugFrame.image_width = image.cols;
            debugFrame.image_height = image.rows;
            const Point2f imageCenter = detector.getImageCenter();
            debugFrame.image_center_x = imageCenter.x;
            debugFrame.image_center_y = imageCenter.y;
            debugFrame.turn_flag = target.turnFlag;
            debugFrame.distance_flag = target.distanceFlag;
            debugFrame.tx_sent = txSent;
            debugFrame.tx_turn_flag = txTurn;
            debugFrame.tx_distance_flag = txDist;
            debugFrame.tx_clear_flag = txClear;
            debugFrame.tracking = detector.isTracking();
            debugFrame.lock_frames = detector.getConsecutiveFrames();
            debugFrame.required_frames = detector.getRequiredFrames();
            debugFrame.lost_frames = detector.getLostFrames();
            debugFrame.required_lost_frames = detector.getRequiredLostFrames();
            debugFrame.green_pixels = detector.getGreenPixelCount();
            debugFrame.candidate_count = detector.getCandidateCount();
            debugFrame.fps = fpsEma;
            debugFrame.processing_time_ms = processingTimeMs;
            debugFrame.source = webOptions.source;
            debugFrame.source_label = webOptions.source_label;
            debugFrame.mode = webOptions.mode;
            telemetry.publish(debugFrame);
        }

        if (cli.showGui)
        {
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
        }

        if (videoWriter.isOpened() && !displayImage.empty())
        {
            videoWriter.write(displayImage);
        }

        if (cli.showGui)
        {
            if (waitKey(1) == 27)
            {
                break;
            }
        }
        else
        {
            // 无 GUI 时用 1ms 休眠近似原 waitKey(1) 的节流行为
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    telemetry.stop();
    serial.Close();
    if (cli.showGui)
        destroyAllWindows();

    restoreTerminalOutput();

    return 0;
}

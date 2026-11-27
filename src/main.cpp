/**
 * @file main.cpp
 * @brief 飞镖制导系统主程序
 * @author lxy
 * @date 2025-10-24
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <opencv2/highgui.hpp>
#include <opencv2/opencv.hpp>
#include <yaml-cpp/yaml.h>
#include "io/camera/HaiKang.h"
#include "io/serial/VirtualSerial.h"
#include "tasks/dart/GreenDetect.h"
#include "tasks/dart/DartDetector.h"
#include "tasks/dart/DartTracker.h"
#include "tasks/dart/Observation.h"
#include "tools/Utils.h"
#include "tools/PerformanceStats.h"
#include "tools/WebPerformanceWindow.h"
#include "debug/DebugTelemetry.h"
#include "debug/FrameSource.h"
#include "debug/Recorder.h"
#include "debug/RuntimeParams.h"

using namespace cv;
using namespace std;

namespace
{
struct DartDetectionResult
{
    std::vector<dartvision::Candidate> candidates;
    double elapsedMs = 0.0;
    dartvision::DetectionDiagnostics diagnostics;
};

// 单个长期运行的工作线程；每帧汇合后才提交下一帧，避免乱序和积压。
class DartDetectionWorker
{
public:
    DartDetectionWorker() : thread_([this] { run(); }) {}
    ~DartDetectionWorker()
    {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        condition_.notify_one();
        thread_.join();
    }
    std::future<DartDetectionResult> submit(const cv::Mat &image, dartvision::DartDetector detector)
    {
        std::packaged_task<DartDetectionResult()> task([image, detector = std::move(detector)] {
            const auto start = std::chrono::steady_clock::now();
            dartvision::DetectionDiagnostics diagnostics;
            auto candidates = detector.detect(image, &diagnostics);
            return DartDetectionResult{std::move(candidates),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(), diagnostics};
        });
        auto result = task.get_future();
        { std::lock_guard<std::mutex> lock(mutex_); pending_ = std::move(task); }
        condition_.notify_one();
        return result;
    }
private:
    void run()
    {
        for (;;) {
            std::packaged_task<DartDetectionResult()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] { return stop_ || pending_.valid(); });
                if (stop_ && !pending_.valid()) return;
                task = std::move(pending_);
            }
            task();
        }
    }
    std::mutex mutex_;
    std::condition_variable condition_;
    std::packaged_task<DartDetectionResult()> pending_;
    bool stop_ = false;
    std::thread thread_;
};

std::atomic<bool> g_shutdown_requested{false};

/** 信号处理：只置退出标志（async-signal-safe），由主循环完成后台线程收尾 */
void handleShutdownSignal(int)
{
    g_shutdown_requested.store(true);
}

void installSignalHandlers()
{
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handleShutdownSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // 不设置 SA_RESTART：让阻塞中的 waitKey 等调用被中断并回到主循环
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

/**
 * @brief 调试链路命令行参数（全部可选）
 *
 * 默认使用海康相机、串口和录制系统，本地 GUI 按需开启。
 */
struct CliOptions
{
    std::string sourceSpec;        // 空 = 海康工业相机；synthetic / image:<path> / video:<path>
    bool forceSimSerial = false;   // --no-serial: 跳过串口打开，仅模拟发送
    bool showGui = false;          // --gui: 按需显示 OpenCV 调试窗口
    bool recordEnabled = true;     // --record / --no-record: 录制系统开关（覆盖配置）
    int autoRecordOverride = -1;   // -1=跟随配置, 0=不自动录制, 1=启动即录制（覆盖配置）
    std::string recordRoot;        // --record-root: 录制会话根目录（覆盖配置）
    std::string recordImageMode;   // --record-image-mode: 自动录像来源
    int webDebugOverride = -1;     // -1=跟随配置文件, 0=强制关闭, 1=强制开启
    bool perf = false;
    bool parallelDetection = false;
};

void printUsage(const char *prog)
{
    cout << "飞镖制导系统（含 M1 Web 调试 + M2 数据录制）\n"
         << "用法: " << prog << " [选项]\n"
         << "  --source <spec>     使用模拟视频源（默认使用海康工业相机）\n"
         << "                      spec: synthetic | synthetic:none | image:<path> | video:<path>\n"
         << "  --no-serial         不打开串口，使用模拟发送（无电控环境测试）\n"
         << "  --gui               显示 OpenCV 调试窗口（默认关闭）\n"
         << "  --no-gui            不创建 OpenCV 显示窗口\n"
         << "  --record            启用录制（覆盖配置 recording.enabled）\n"
         << "  --no-record         关闭录制（不生成会话目录/CSV/视频）\n"
         << "  --auto-record       程序启动即开始录制（覆盖配置 recording.auto_start）\n"
         << "  --no-auto-record    程序启动不自动录制（预览调试时手动开始，覆盖配置）\n"
         << "  --record-root <dir> 指定录制会话根目录（覆盖配置 recording.root）\n"
         << "  --record-image-mode <raw|debug> 自动录像来源\n"
         << "  --web-debug         强制启用 Web 调试数据发布（覆盖配置）\n"
         << "  --no-web-debug      强制关闭 Web 调试数据发布（覆盖配置）\n"
         << "  --perf              每 5 秒输出一次处理阶段性能统计\n"
         << "  --parallel-detection 并行绿灯与飞镖检测（实验选项）\n"
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
        else if (arg == "--gui")
            opt.showGui = true;
        else if (arg == "--no-gui")
            opt.showGui = false;
        else if (arg == "--record")
            opt.recordEnabled = true;
        else if (arg == "--no-record")
            opt.recordEnabled = false;
        else if (arg == "--auto-record")
            opt.autoRecordOverride = 1;
        else if (arg == "--no-auto-record")
            opt.autoRecordOverride = 0;
        else if (arg == "--record-root" && i + 1 < argc)
            opt.recordRoot = argv[++i];
        else if (arg == "--record-image-mode" && i + 1 < argc)
            opt.recordImageMode = argv[++i];
        else if (arg == "--web-debug")
            opt.webDebugOverride = 1;
        else if (arg == "--no-web-debug")
            opt.webDebugOverride = 0;
        else if (arg == "--perf")
            opt.perf = true;
        else if (arg == "--parallel-detection")
            opt.parallelDetection = true;
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
    installSignalHandlers();

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
    int yawTimeoutMs = 1000; // 临时值，需与电控上报周期协商
    int terminalLogLineLimit = 50;
    int brightThresh = 80;
    int greenWeightX100 = 40; // green_weight_thresh * 100
    int exposure = 3200, gain = 10;
    int circularityX100 = 60;
    int minAspX100 = 50, maxAspX100 = 200;
    int minArea = 50, maxArea = 50000;
    int minRadius = 10, maxRadius = 30;
    // M3：可热更新参数的初值（来自 YAML，YAML 缺失时使用算法内置默认值）
    int requiredFrames = 10;
    int requiredLostFrames = 10;
    double maxCenterDist = 100.0;
    int kernelSize = 5;
    int openIterations = 1;
    int closeIterations = 1;
    bool dartEnabled = false, dartAnnotate = true;
    dartvision::DetectorConfig dartDetectorConfig;
    dartvision::TrackerConfig dartTrackerConfig;
    std::string loadedConfigPath;
    std::map<std::string, double> p3Initial;

    // Web 调试数据发布配置（config/debug.yaml 的 web_debug 段，可被命令行覆盖）
    bool webDebugEnabled = true;
    std::string webDebugHost = "127.0.0.1";
    int webDebugPort = 5010;
    int webDebugPublishFps = 20;
    int webDebugJpegQuality = 75;
    std::string cameraSerialNumber = "unknown";

    // M2 数据录制配置（config/debug.yaml 的 recording 段，可被命令行覆盖）
    bool recordEnabled = true;
    bool recordAutoStart = true;
    std::string recordRoot = "recordings";
    std::string recordImageMode = "debug";
    int recordVideoScale = 1;
    double recordVideoFps = 30.0;
    int recordQueueCapacity = 64;
    int recordControlPort = 5011;

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

        // P3 新字段缺失时使用内置默认；不要求旧录像配置升级。
        auto readP3 = [&](const YAML::Node &section, const std::string &prefix,
                          std::initializer_list<const char *> keys) {
            if (!section || !section.IsMap()) return;
            for (const auto *key : keys) if (section[key]) {
                const auto text = section[key].as<std::string>();
                if (text == "true" || text == "false") p3Initial[prefix + key] = text == "true" ? 1.0 : 0.0;
                else p3Initial[prefix + key] = section[key].as<double>();
            }
        };
        if (config["detection"]) readP3(config["detection"]["roi"], "green_roi_", {"enabled", "x", "y", "width", "height"});
        if (config["dart_detection"]) {
            const auto d = config["dart_detection"];
            readP3(d["roi"], "dart_roi_", {"enabled", "x", "y", "width", "height"});
            readP3(d["circularity"], "dart_circularity_", {"enabled", "max"});
            readP3(d["direction"], "dart_direction_", {"enabled", "points", "min_displacement"});
            readP3(d["window"], "dart_window_", {"enabled", "delay_ms", "duration_ms", "tracking_ms"});
            readP3(d["reference_line"], "dart_reference_", {"enabled", "x1", "y1", "x2", "y2"});
        }

        if (config["dart_detection"])
        {
            try
            {
                const auto d = config["dart_detection"];
                if (d["enabled"]) dartEnabled = d["enabled"].as<bool>();
                if (d["draw_debug"]) dartAnnotate = d["draw_debug"].as<bool>();
                if (d["color"]) dartDetectorConfig.color = d["color"].as<std::string>();
                if (d["saturation_min"]) dartDetectorConfig.saturation_min = d["saturation_min"].as<int>();
                if (d["value_min"]) dartDetectorConfig.value_min = d["value_min"].as<int>();
                if (d["red_low_max"]) dartDetectorConfig.red_low_max = d["red_low_max"].as<int>();
                if (d["red_high_min"]) dartDetectorConfig.red_high_min = d["red_high_min"].as<int>();
                if (d["blue_hue_min"]) dartDetectorConfig.blue_hue_min = d["blue_hue_min"].as<int>();
                if (d["blue_hue_max"]) dartDetectorConfig.blue_hue_max = d["blue_hue_max"].as<int>();
                if (d["min_area"]) dartDetectorConfig.min_area = d["min_area"].as<double>();
                if (d["max_area"]) dartDetectorConfig.max_area = d["max_area"].as<double>();
                if (d["min_aspect"]) dartDetectorConfig.min_aspect = d["min_aspect"].as<double>();
                if (d["max_aspect"]) dartDetectorConfig.max_aspect = d["max_aspect"].as<double>();
                if (d["close_kernel"]) dartDetectorConfig.close_kernel = d["close_kernel"].as<int>();
                if (d["max_distance"]) dartTrackerConfig.max_distance = d["max_distance"].as<double>();
                if (d["min_motion"]) dartTrackerConfig.min_motion = d["min_motion"].as<double>();
                if (d["max_lost_frames"]) dartTrackerConfig.max_lost_frames = d["max_lost_frames"].as<int>();
                if (d["max_track_points"]) dartTrackerConfig.max_points = d["max_track_points"].as<int>();
                const auto &c = dartDetectorConfig;
                if ((c.color != "red" && c.color != "blue") || c.saturation_min < 0 || c.saturation_min > 255 ||
                    c.value_min < 0 || c.value_min > 255 || c.red_low_max < 0 || c.red_low_max > 179 ||
                    c.red_high_min < 0 || c.red_high_min > 179 || c.red_low_max >= c.red_high_min ||
                    c.blue_hue_min < 0 || c.blue_hue_max > 179 || c.blue_hue_min > c.blue_hue_max ||
                    !std::isfinite(c.min_area) || !std::isfinite(c.max_area) || c.min_area < 0 || c.max_area < c.min_area ||
                    !std::isfinite(c.min_aspect) || !std::isfinite(c.max_aspect) || c.min_aspect <= 0 || c.max_aspect < c.min_aspect ||
                    c.close_kernel < 1 || c.close_kernel > 31 || c.close_kernel % 2 == 0 ||
                    !std::isfinite(dartTrackerConfig.max_distance) || !std::isfinite(dartTrackerConfig.min_motion) ||
                    dartTrackerConfig.min_motion <= 0 || dartTrackerConfig.max_distance < dartTrackerConfig.min_motion ||
                    dartTrackerConfig.max_lost_frames < 0 || dartTrackerConfig.max_lost_frames > 1000 ||
                    dartTrackerConfig.max_points < 2 || dartTrackerConfig.max_points > 1000)
                    throw std::runtime_error("飞镖检测参数无效或超出范围");
            }
            catch (const std::exception &e)
            {
                dartEnabled = false;
                dartDetectorConfig = dartvision::DetectorConfig{};
                dartTrackerConfig = dartvision::TrackerConfig{};
                dartAnnotate = true;
                cerr << "[Dart] 配置无效，已关闭飞镖检测: " << e.what() << endl;
            }
        }

        if (config["turn_control"] && config["turn_control"]["turn_threshold"])
            turnThreshold = config["turn_control"]["turn_threshold"].as<double>();
        if (config["turn_control"] && config["turn_control"]["dist_far_thresh"])
            distFarThresh = config["turn_control"]["dist_far_thresh"].as<double>();
        if (config["turn_control"] && config["turn_control"]["dist_near_thresh"])
            distNearThresh = config["turn_control"]["dist_near_thresh"].as<double>();
        if (config["communication"] && config["communication"]["port"])
            serialPort = config["communication"]["port"].as<std::string>();
        if (config["communication"] && config["communication"]["yaw_timeout_ms"])
            yawTimeoutMs = std::clamp(config["communication"]["yaw_timeout_ms"].as<int>(), 1, 600000);
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
        // M2：数据录制配置（可选段，缺省时使用内置默认值）
        if (config["recording"])
        {
            auto rc = config["recording"];
            if (rc["enabled"])
                recordEnabled = rc["enabled"].as<bool>();
            if (rc["auto_start"])
                recordAutoStart = rc["auto_start"].as<bool>();
            if (rc["root"])
                recordRoot = rc["root"].as<std::string>();
            if (rc["record_image_mode"])
                recordImageMode = rc["record_image_mode"].as<std::string>();
            if (rc["video_scale"])
                recordVideoScale = rc["video_scale"].as<int>();
            if (rc["video_fps"])
                recordVideoFps = rc["video_fps"].as<double>();
            if (rc["queue_capacity"])
                recordQueueCapacity = rc["queue_capacity"].as<int>();
            if (rc["control_port"])
                recordControlPort = rc["control_port"].as<int>();
        }
        if (config["detection"] && config["detection"]["time_filter"])
        {
            auto tf = config["detection"]["time_filter"];
            if (tf["required_frames"])
                requiredFrames = tf["required_frames"].as<int>();
            if (tf["required_lost_frames"])
                requiredLostFrames = tf["required_lost_frames"].as<int>();
            if (tf["max_center_dist"])
                maxCenterDist = tf["max_center_dist"].as<double>();
        }
        if (config["detection"] && config["detection"]["morphology"])
        {
            auto mo = config["detection"]["morphology"];
            if (mo["kernel_size"])
                kernelSize = mo["kernel_size"].as<int>();
            if (mo["open_iterations"])
                openIterations = mo["open_iterations"].as<int>();
            if (mo["close_iterations"])
                closeIterations = mo["close_iterations"].as<int>();
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

    // M3：运行时参数服务。C++ 运行值是实际生效值的权威来源：
    // 网页 / 控制口只通过 RuntimeParams 修改，视觉线程每帧取一致快照。
    webdebug::RuntimeParams runtimeParams;
    {
        std::map<std::string, double> initial = {
            {"bright_thresh", static_cast<double>(brightThresh)},
            {"green_weight_thresh", greenWeightX100 / 100.0},
            {"min_area", static_cast<double>(minArea)},
            {"max_area", static_cast<double>(maxArea)},
            {"min_radius", static_cast<double>(minRadius)},
            {"max_radius", static_cast<double>(maxRadius)},
            {"min_circularity", circularityX100 / 100.0},
            {"min_aspect_ratio", minAspX100 / 100.0},
            {"max_aspect_ratio", maxAspX100 / 100.0},
            {"kernel_size", static_cast<double>(kernelSize)},
            {"open_iterations", static_cast<double>(openIterations)},
            {"close_iterations", static_cast<double>(closeIterations)},
            {"required_frames", static_cast<double>(requiredFrames)},
            {"required_lost_frames", static_cast<double>(requiredLostFrames)},
            {"max_center_dist", maxCenterDist},
            {"turn_threshold", turnThreshold},
            {"dist_far_thresh", distFarThresh},
            {"dist_near_thresh", distNearThresh},
            {"exposure", static_cast<double>(exposure)},
            {"gain", static_cast<double>(gain)},
            {"dart_enabled", static_cast<double>(dartEnabled)},
            {"dart_color", static_cast<double>(dartDetectorConfig.color == "blue")},
            {"dart_draw_debug", static_cast<double>(dartAnnotate)},
            {"dart_saturation_min", static_cast<double>(dartDetectorConfig.saturation_min)},
            {"dart_value_min", static_cast<double>(dartDetectorConfig.value_min)},
            {"dart_red_low_max", static_cast<double>(dartDetectorConfig.red_low_max)},
            {"dart_red_high_min", static_cast<double>(dartDetectorConfig.red_high_min)},
            {"dart_blue_hue_min", static_cast<double>(dartDetectorConfig.blue_hue_min)},
            {"dart_blue_hue_max", static_cast<double>(dartDetectorConfig.blue_hue_max)},
            {"dart_min_area", static_cast<double>(dartDetectorConfig.min_area)},
            {"dart_max_area", static_cast<double>(dartDetectorConfig.max_area)},
            {"dart_min_aspect", static_cast<double>(dartDetectorConfig.min_aspect)},
            {"dart_max_aspect", static_cast<double>(dartDetectorConfig.max_aspect)},
            {"dart_close_kernel", static_cast<double>(dartDetectorConfig.close_kernel)},
            {"dart_max_distance", static_cast<double>(dartTrackerConfig.max_distance)},
            {"dart_min_motion", static_cast<double>(dartTrackerConfig.min_motion)},
            {"dart_max_lost_frames", static_cast<double>(dartTrackerConfig.max_lost_frames)},
            {"dart_max_track_points", static_cast<double>(dartTrackerConfig.max_points)},
        };
        initial.insert(p3Initial.begin(), p3Initial.end());
        // 对新字段在启动时执行范围检查，防止异常浮点数被转换成 ROI 坐标。
        for (const auto &def : runtimeParams.definitions()) {
            if (!p3Initial.count(def.name)) continue;
            const double value = initial.at(def.name);
            if (!std::isfinite(value) || value < def.min_value || value > def.max_value ||
                (def.is_int && std::floor(value) != value)) {
                std::cerr << "[P3] 无效参数 " << def.name << "，使用安全默认值" << std::endl;
                initial.erase(def.name);
            }
        }
        runtimeParams.initialize(initial);
        if (!loadedConfigPath.empty())
            runtimeParams.setConfigPath(std::filesystem::absolute(loadedConfigPath).string());
    }

    const std::string runTimestamp = makeRunTimestamp();
    const auto projectRootDir = resolveProjectRoot(loadedConfigPath);
    const auto logRootDir = projectRootDir / "logs";
    const auto runLogDir = logRootDir / runTimestamp;
    std::filesystem::create_directories(runLogDir);
    pruneRunLogs(logRootDir, runLogDir);

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
    dartvision::DartDetector dartDetector(dartDetectorConfig);
    dartvision::DartTracker dartTracker(dartTrackerConfig);
    dartvision::FlightWindow flightWindow;
    bool dartTrackActive = false;
    uint64_t simulatedFireEventId = 0;
    uint64_t observationTrackId = 0;
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

    // M3：GUI Trackbar 与运行时参数统一绑定。
    // Trackbar 只是控件：拖动时写回 RuntimeParams（来源 trackbar），网页修改后反向刷新 Trackbar 显示；
    // 两者的运行值始终只有 RuntimeParams 一份，避免"网页与 Trackbar 各自维护不同运行值"。
    struct TrackbarBinding
    {
        const char *param;
        const char *label;
        int *value;
        double scale; // 参数值 = Trackbar 整数 / scale
        int min_trackbar;
        int max_trackbar;
    };
    std::vector<TrackbarBinding> trackbarBindings = {
        {"bright_thresh", "Bright(0-255)", &brightThresh, 1.0, 0, 255},
        {"green_weight_thresh", "Green% x100", &greenWeightX100, 100.0, 0, 100},
        {"min_circularity", "Circ% x100", &circularityX100, 100.0, 0, 100},
        {"min_aspect_ratio", "Asp_min x100", &minAspX100, 100.0, 0, 300},
        {"max_aspect_ratio", "Asp_max x100", &maxAspX100, 100.0, 0, 500},
        {"min_area", "Area_min(px2)", &minArea, 1.0, 0, 10000},
        {"max_area", "Area_max(px2)", &maxArea, 1.0, 0, 100000},
        {"min_radius", "Radius_min(px)", &minRadius, 1.0, 0, 200},
        {"max_radius", "Radius_max(px)", &maxRadius, 1.0, 0, 200},
        {"exposure", "Exposure(us)", &exposure, 1.0, 0, 100000},
        {"gain", "Gain", &gain, 1.0, 0, 100},
    };
    std::map<std::string, int> lastTrackbarValue;
    if (cli.showGui)
    {
        for (const auto &binding : trackbarBindings)
            lastTrackbarValue[binding.param] = *binding.value;
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

    // 5. M2 数据录制（独立后台线程 + 会话目录；不依赖 Flask / 浏览器）
    if (!cli.recordEnabled)
        recordEnabled = false;
    if (cli.autoRecordOverride >= 0)
        recordAutoStart = (cli.autoRecordOverride == 1);
    if (!cli.recordRoot.empty())
        recordRoot = cli.recordRoot;
    if (!cli.recordImageMode.empty()) recordImageMode = cli.recordImageMode;
    if (recordImageMode != "raw" && recordImageMode != "debug") {
        cerr << "[Recorder] record_image_mode 必须是 raw 或 debug" << endl;
        return 2;
    }

    webdebug::RecordOptions recordOptions;
    recordOptions.enabled = recordEnabled;
    recordOptions.auto_start = recordAutoStart;
    {
        std::filesystem::path root(recordRoot);
        if (root.is_relative())
            root = projectRootDir / root;
        recordOptions.root = root.lexically_normal().string();
    }
    recordOptions.video_scale = recordVideoScale;
    recordOptions.video_fps = recordVideoFps;
    recordOptions.queue_capacity = recordQueueCapacity;
    recordOptions.control_port = recordControlPort;
    recordOptions.source = webOptions.source;
    recordOptions.source_label = webOptions.source_label;
    recordOptions.mode = webOptions.mode;
    recordOptions.record_image_mode = recordImageMode;
    recordOptions.config_path = std::filesystem::absolute(loadedConfigPath).string();
    // 启动时快照仅作回退；实际每个录制会话开始时由 Recorder 从 RuntimeParams 取当前值
    recordOptions.runtime_params_json = runtimeParams.runningJson();
    recordOptions.app_version = "DART-WEB-M3 (base M2 commit 9adb3f0)";
    std::atomic<unsigned> dartResetRequests{0};
    std::atomic<unsigned> dartSimulatedFireRequests{0};
    webdebug::Recorder recorder;
    recorder.setDartResetRequest([&dartResetRequests]() { dartResetRequests.fetch_add(1); });
    recorder.setDartSimulatedFireRequest([&dartSimulatedFireRequests]() { dartSimulatedFireRequests.fetch_add(1); });
    recorder.setRuntimeParams(&runtimeParams);
    // 参数实际生效 -> 若正在录制则追加 events.jsonl（未录制时不写事件，只更新运行参数）
    runtimeParams.setEventSink([&recorder](const webdebug::ParamChangeEvent &event) {
        recorder.appendParameterEvent(event);
    });
    recorder.configure(recordOptions);
    recorder.start();

    // 6. 主循环
    Mat image;
    Mat resizedImage;
    Mat displayImage;
    const int DISPLAY_SCALE = 2;
    const int RESULT_SCALE = 3;

    Size originalSize;
    Size displaySize;
    Size resultSize;

    auto lastFrameTime = std::chrono::steady_clock::now();
    double fpsEma = 0.0;
    uint64_t frameCounter = 0;
    uint64_t appliedDartRevision = 0;
    std::unique_ptr<DartDetectionWorker> dartWorker;
    if (cli.parallelDetection) dartWorker = std::make_unique<DartDetectionWorker>();
    PerformanceStats perf(cli.perf);
    detector.enablePerformanceProfiling(cli.perf);
    if (cli.perf) cout << "[GreenPerf] G01/G02/G04 合并计入 G03_fused_color" << endl;
    detector.setDebugRendering(cli.showGui || webDebugEnabled || recordEnabled);
    if (camera) camera->enableDiagnostics(cli.perf || webDebugEnabled);
    WebPerformanceWindow webPerformance(PerformanceStats::Clock::now());
    std::optional<std::uint32_t> lastWebCameraFrame;
    std::uint64_t lastWebCameraReconnect = 0;
    auto lastCameraStatsTime = PerformanceStats::Clock::now();
    std::uint64_t lastReceived = 0;
    auto printCameraStats = [&]() {
        if (!camera || !perf.enabled()) return;
        const auto now = PerformanceStats::Clock::now();
        const auto d = camera->diagnostics();
        const double seconds = std::chrono::duration<double>(now - lastCameraStatsTime).count();
        std::cout << "[Perf] host_receive_fps=" << (seconds > 0 ? (d.received - lastReceived) / seconds : 0)
                  << " received=" << d.received << " frame=" << d.frame_number
                  << " device_timestamp_raw=" << d.device_timestamp << " size=" << d.width << "x" << d.height
                  << " gaps=" << d.gaps << " missed=" << d.missed << " frame_resets=" << d.resets
                  << " frame_wraps=" << d.frame_wraps << " timestamp_backwards=" << d.timestamp_backwards
                  << " failures=" << d.failures << " reconnects=" << d.reconnects
                  << " exposure_us=" << camera->actualExposure() << " gain=" << camera->actualGain() << std::endl;
        lastCameraStatsTime = now;
        lastReceived = d.received;
    };

    while (!g_shutdown_requested.load())
    {
        const auto perfLoopStart = PerformanceStats::Clock::now();
        if (serialWatchdog && !serial.IsSimulated() && !serial.IsOpen())
        {
            cerr << "[Main] Serial port disconnected, exiting for watchdog restart." << endl;
            return 1;
        }

        // M3（1/3）：GUI Trackbar 拖动 -> 运行时参数（Trackbar 只是控件，权威值始终在 RuntimeParams）
        if (cli.showGui)
        {
            for (const auto &binding : trackbarBindings)
            {
                auto last = lastTrackbarValue.find(binding.param);
                const int current = *binding.value;
                if (last == lastTrackbarValue.end() || last->second == current)
                    continue;
                last->second = current;
                std::string error;
                std::vector<std::pair<std::string, std::pair<double, double>>> changes;
                runtimeParams.applyValues({{binding.param, current / binding.scale}}, "trackbar",
                                          &error, &changes);
                if (!error.empty())
                    std::cout << "[Params] Trackbar 修改被拒绝: " << error << std::endl;
            }
        }

        // M3（2/3）：每帧取一次完整参数快照，保证同一帧处理过程中参数一致
        uint64_t dartRevision = 0;
        const webdebug::RuntimeParams::Snapshot runtimeSnapshot = runtimeParams.snapshot(&dartRevision);
        auto runtimeValue = [&runtimeSnapshot](const char *name) -> double {
            auto it = runtimeSnapshot.find(name);
            return (it == runtimeSnapshot.end()) ? 0.0 : it->second;
        };
        exposure = static_cast<int>(std::lround(runtimeValue("exposure")));
        gain = static_cast<int>(std::lround(runtimeValue("gain")));

        // M3（3/3）：网页修改后刷新 Trackbar 显示；控件范围只影响显示，不回写运行值
        if (cli.showGui)
        {
            for (const auto &binding : trackbarBindings)
            {
                const int expected = std::clamp(
                    static_cast<int>(std::lround(runtimeValue(binding.param) * binding.scale)),
                    binding.min_trackbar, binding.max_trackbar);
                if (expected != *binding.value)
                {
                    *binding.value = expected;
                    cv::setTrackbarPos(binding.label, "BGR Control", expected);
                    lastTrackbarValue[binding.param] = expected;
                }
            }
        }

        if (camera)
        {
            camera->setCameraExposureGain(exposure, gain);
            const auto captureStart = PerformanceStats::Clock::now();
            camera->getimage(image);
            if (webDebugEnabled && !image.empty()) {
                const auto d = camera->diagnostics();
                if (d.valid && (!lastWebCameraFrame || *lastWebCameraFrame != d.frame_number ||
                                lastWebCameraReconnect != d.reconnects)) {
                    webPerformance.received(PerformanceStats::Clock::now());
                    lastWebCameraFrame = d.frame_number;
                    lastWebCameraReconnect = d.reconnects;
                }
            }
            perf.add(perf.capture, PerformanceStats::ms(captureStart, PerformanceStats::Clock::now()));
            if (perf.enabled())
            {
                const auto d = camera->diagnostics();
                if (d.attempted) perf.add(perf.sdk_wait, d.wait_ms);
                if (d.valid) perf.add(perf.conversion, d.convert_ms);
            }
        }
        else
        {
            const auto captureStart = PerformanceStats::Clock::now();
            if (!frameSource->next(image))
            {
                if (cli.showGui)
                    waitKey(1);
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            perf.add(perf.capture, PerformanceStats::ms(captureStart, PerformanceStats::Clock::now()));
        }
        if (image.empty())
        {
            if (perf.flush()) printCameraStats();
            continue;
        }

        if (originalSize.width == 0)
        {
            originalSize = image.size();
            displaySize = Size(originalSize.width / DISPLAY_SCALE, originalSize.height / DISPLAY_SCALE);
            resultSize = Size(originalSize.width * RESULT_SCALE, originalSize.height * RESULT_SCALE);
        }

        // M3：把本帧参数快照同步到识别器（参数不变时与 M2 行为完全一致）
        detector.setRoi(runtimeValue("green_roi_enabled") != 0,
            cv::Rect(static_cast<int>(runtimeValue("green_roi_x")), static_cast<int>(runtimeValue("green_roi_y")),
                     static_cast<int>(runtimeValue("green_roi_width")), static_cast<int>(runtimeValue("green_roi_height"))));
        detector.setBrightThresh(static_cast<int>(std::lround(runtimeValue("bright_thresh"))));
        detector.setGreenWeightThresh(static_cast<float>(runtimeValue("green_weight_thresh")));
        detector.setShapeFilter(runtimeValue("min_circularity"), runtimeValue("min_aspect_ratio"),
                                runtimeValue("max_aspect_ratio"));
        detector.setAreaFilter(runtimeValue("min_area"), runtimeValue("max_area"));
        detector.setRadiusFilter(static_cast<int>(std::lround(runtimeValue("min_radius"))),
                                 static_cast<int>(std::lround(runtimeValue("max_radius"))));
        detector.setTurnThreshold(static_cast<float>(runtimeValue("turn_threshold")));
        detector.setDistanceThresholds(static_cast<float>(runtimeValue("dist_far_thresh")),
                                       static_cast<float>(runtimeValue("dist_near_thresh")));
        detector.setRequiredFrames(static_cast<int>(std::lround(runtimeValue("required_frames"))));
        detector.setRequiredLostFrames(
            static_cast<int>(std::lround(runtimeValue("required_lost_frames"))));
        detector.setMaxCenterDist(static_cast<float>(runtimeValue("max_center_dist")));
        detector.setMorphology(static_cast<int>(std::lround(runtimeValue("kernel_size"))),
                               static_cast<int>(std::lround(runtimeValue("open_iterations"))),
                               static_cast<int>(std::lround(runtimeValue("close_iterations"))));

        const bool resetDart = dartRevision != appliedDartRevision;
        appliedDartRevision = dartRevision;
        dartEnabled = runtimeValue("dart_enabled") != 0;
        dartAnnotate = runtimeValue("dart_draw_debug") != 0;
        dartDetectorConfig.roi_enabled = runtimeValue("dart_roi_enabled") != 0;
        dartDetectorConfig.roi = cv::Rect(static_cast<int>(runtimeValue("dart_roi_x")), static_cast<int>(runtimeValue("dart_roi_y")),
                                         static_cast<int>(runtimeValue("dart_roi_width")), static_cast<int>(runtimeValue("dart_roi_height")));
        dartDetectorConfig.circularity_filter_enabled = runtimeValue("dart_circularity_enabled") != 0;
        dartDetectorConfig.max_circularity = runtimeValue("dart_circularity_max");
        dartTrackerConfig.direction_enabled = runtimeValue("dart_direction_enabled") != 0;
        dartTrackerConfig.direction_observation_points = static_cast<int>(runtimeValue("dart_direction_points"));
        dartTrackerConfig.direction_min_displacement = runtimeValue("dart_direction_min_displacement");
        dartDetectorConfig.color = runtimeValue("dart_color") == 1 ? "blue" : "red";
        dartDetectorConfig.saturation_min = static_cast<int>(runtimeValue("dart_saturation_min"));
        dartDetectorConfig.value_min = static_cast<int>(runtimeValue("dart_value_min"));
        dartDetectorConfig.red_low_max = static_cast<int>(runtimeValue("dart_red_low_max"));
        dartDetectorConfig.red_high_min = static_cast<int>(runtimeValue("dart_red_high_min"));
        dartDetectorConfig.blue_hue_min = static_cast<int>(runtimeValue("dart_blue_hue_min"));
        dartDetectorConfig.blue_hue_max = static_cast<int>(runtimeValue("dart_blue_hue_max"));
        dartDetectorConfig.min_area = runtimeValue("dart_min_area");
        dartDetectorConfig.max_area = runtimeValue("dart_max_area");
        dartDetectorConfig.min_aspect = runtimeValue("dart_min_aspect");
        dartDetectorConfig.max_aspect = runtimeValue("dart_max_aspect");
        dartDetectorConfig.close_kernel = static_cast<int>(runtimeValue("dart_close_kernel"));
        dartTrackerConfig.max_distance = runtimeValue("dart_max_distance");
        dartTrackerConfig.min_motion = runtimeValue("dart_min_motion");
        dartTrackerConfig.max_lost_frames = static_cast<int>(runtimeValue("dart_max_lost_frames"));
        dartTrackerConfig.max_points = static_cast<int>(runtimeValue("dart_max_track_points"));
        dartDetector = dartvision::DartDetector(dartDetectorConfig);
        const int64_t windowNow = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool windowEnabled = runtimeValue("dart_window_enabled") != 0;
        flightWindow.configure({windowEnabled, static_cast<int64_t>(runtimeValue("dart_window_delay_ms")),
                                static_cast<int64_t>(runtimeValue("dart_window_duration_ms")),
                                static_cast<int64_t>(runtimeValue("dart_window_tracking_ms"))});
        if (resetDart) { dartTracker = dartvision::DartTracker(dartTrackerConfig); dartTrackActive = false; }
        const auto fireRequests = dartSimulatedFireRequests.exchange(0);
        if (fireRequests && windowEnabled) {
            flightWindow.trigger(++simulatedFireEventId, windowNow);
            dartTracker.reset(); dartTrackActive = false;
        }
        if (dartTrackActive && !flightWindow.allowsExisting(windowNow)) {
            dartTracker.reset(); dartTrackActive = false;
        }
        const bool runDart = dartEnabled && (dartTrackActive ? flightWindow.allowsExisting(windowNow) : flightWindow.allowsNew(windowNow));
        if (dartEnabled && !runDart) dartTracker.reset();
        std::future<DartDetectionResult> dartFuture;
        if (dartWorker && runDart)
            dartFuture = dartWorker->submit(image, dartDetector);

        // 主循环帧率（含采集/显示开销）与识别处理耗时：来自实际运行，非估算
        const auto frameStartTime = std::chrono::steady_clock::now();
        auto target = detector.detectGreenTarget(image);
        const auto detectEndTime = std::chrono::steady_clock::now();
        const double processingTimeMs =
            std::chrono::duration<double, std::milli>(detectEndTime - frameStartTime).count();
        perf.add(perf.green, processingTimeMs);
        const int64_t frameTimestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                             detectEndTime.time_since_epoch())
                                             .count();
        const auto yaw = serial.PollYawFeedback(yawTimeoutMs);
        const std::string yawStatus = !serial.IsOpen() || serial.IsSimulated() ? "disconnected" :
            !yaw.seen ? "waiting" : yaw.valid ? "valid" :
            (std::chrono::steady_clock::now() - yaw.received_at > std::chrono::milliseconds(yawTimeoutMs) ? "stale" : "invalid");
        const uint64_t frameId = ++frameCounter;
        const auto manualResets = dartResetRequests.exchange(0);
        if (manualResets) { dartTracker.reset(); dartTrackActive = false; }
        dartvision::TrackResult dartResult;
        dartvision::ReferenceLine referenceLine{"A", {runtimeValue("dart_reference_x1"), runtimeValue("dart_reference_y1")},
                                                     {runtimeValue("dart_reference_x2"), runtimeValue("dart_reference_y2")}};
        const bool referenceEnabled = runtimeValue("dart_reference_enabled") != 0;
        dartvision::Crossing crossing;
        crossing.line_id = "A";
        crossing.reason = referenceEnabled ? "no_confirmed_track" : "disabled";
        std::vector<dartvision::Candidate> dartCandidates;
        dartvision::DetectionDiagnostics dartDiagnostics;
        const auto overlayStart = PerformanceStats::Clock::now();
        cv::Mat annotatedDebug = detector.getDebugImage();
        double dartProcessingMs = 0.0;
        double dartBlockedMs = 0.0;
        if (runDart)
        {
            const auto blockedStart = std::chrono::steady_clock::now();
            double detectionMs = 0.0;
            if (dartWorker) {
                auto detection = dartFuture.get();
                dartCandidates = std::move(detection.candidates);
                dartDiagnostics = detection.diagnostics;
                detectionMs = detection.elapsedMs;
            } else {
                const auto dartStart = std::chrono::steady_clock::now();
                dartCandidates = dartDetector.detect(image, &dartDiagnostics);
                detectionMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - dartStart).count();
            }
            const auto trackStart = std::chrono::steady_clock::now();
            if (frameSource && frameSource->looped()) dartTracker.reset();
            dartResult = dartTracker.update(dartCandidates, frameId, frameTimestampUs,
                                             frameSource ? frameSource->sourceTimeMs() : std::nullopt);
            if (dartResult.track_state == "captured") ++observationTrackId;
            if (referenceEnabled && dartResult.detected && dartResult.track_valid && dartResult.track_points.size() >= 2) {
                const auto &points = dartResult.track_points;
                const bool inside = referenceLine.a.x < image.cols && referenceLine.b.x < image.cols &&
                                    referenceLine.a.y < image.rows && referenceLine.b.y < image.rows;
                if (inside) crossing = dartvision::measureCrossing(referenceLine, points[points.size()-2], points.back());
                else crossing.reason = "line_outside_image";
            }
            if (dartResult.track_valid && !dartTrackActive) flightWindow.established(windowNow);
            dartTrackActive = dartResult.track_valid;
            if (dartDiagnostics.invalid_roi) dartResult.failure_reason = "invalid_roi";
            else if (dartCandidates.empty() && dartDiagnostics.circularity_rejected)
                dartResult.failure_reason = "circularity_rejected";
            dartProcessingMs = detectionMs + std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - trackStart).count();
            dartBlockedMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - blockedStart).count();
            if (dartAnnotate && !annotatedDebug.empty())
            {
                annotatedDebug = annotatedDebug.clone();
                for (const auto &candidate : dartCandidates)
                    cv::rectangle(annotatedDebug, candidate.bbox, cv::Scalar(180, 120, 30), 1);
                if (dartResult.bbox)
                    cv::rectangle(annotatedDebug, *dartResult.bbox, cv::Scalar(0, 255, 255), 2);
                for (std::size_t i = 1; i < dartResult.track_points.size(); ++i)
                    cv::line(annotatedDebug, dartResult.track_points[i-1].center,
                             dartResult.track_points[i].center, cv::Scalar(255, 220, 0), 2);
                cv::putText(annotatedDebug, "Dart: " + dartResult.track_state + " (" +
                            dartResult.failure_reason + ")", cv::Point(10, 90),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(255, 220, 0), 1);
            }
        }
        if (dartEnabled && !runDart) dartResult.failure_reason = "window_closed";
        if (dartEnabled) perf.add(perf.dart, dartProcessingMs);
        if (webDebugEnabled) webPerformance.processed(PerformanceStats::Clock::now(), processingTimeMs + dartProcessingMs);
        perf.add(perf.overlay, std::max(0.0, PerformanceStats::ms(overlayStart, PerformanceStats::Clock::now()) - dartBlockedMs));
        // M3：参数事件记录"生效时最近一帧"的帧号与时间戳（与遥测/视频索引同一时间基准）
        runtimeParams.setFrameContext(frameId, frameTimestampUs);
        {
            const double dt = std::chrono::duration<double>(frameStartTime - lastFrameTime).count();
            lastFrameTime = frameStartTime;
            if (dt > 1e-6)
            {
                const double instantFps = 1.0 / dt;
                fpsEma = (fpsEma <= 0.0) ? instantFps : fpsEma * 0.9 + instantFps * 0.1;
            }
        }

        // 周期性主循环统计（每 5 秒）：无预览/无录制时也能确认算法实际帧率
        {
            static auto lastStatsTime = std::chrono::steady_clock::now();
            static uint64_t lastStatsFrames = 0;
            if (detectEndTime - lastStatsTime > std::chrono::seconds(5))
            {
                const double statsElapsed =
                    std::chrono::duration<double>(detectEndTime - lastStatsTime).count();
                const uint64_t statsDelta = frameId - lastStatsFrames;
                const double loopFps = statsElapsed > 1e-6 ? statsDelta / statsElapsed : 0.0;
                cout << "[Main] loop fps=" << loopFps << " frames=" << frameId
                     << " frame_ms=" << processingTimeMs << endl;
                lastStatsTime = detectEndTime;
                lastStatsFrames = frameId;
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

        const auto submitStart = PerformanceStats::Clock::now();
        // M1：发布本帧调试数据（非阻塞；Flask/浏览器未连接时后台线程直接丢弃）
        {
            webdebug::DebugFrame debugFrame;
            debugFrame.image = annotatedDebug;
            debugFrame.dart_enabled = dartEnabled;
            debugFrame.dart_color = dartDetectorConfig.color;
            debugFrame.dart_processing_ms = dartProcessingMs;
            debugFrame.dart_frame_id = frameId;
            debugFrame.dart_timestamp_us = frameTimestampUs;
            debugFrame.dart_source_time_ms = dartEnabled && frameSource ? frameSource->sourceTimeMs() : std::nullopt;
            debugFrame.dart_result = dartResult;
            debugFrame.dart_crossing = crossing;
            debugFrame.dart_track_id = observationTrackId;
            // 限制单帧网络诊断体积；candidate_count 仍保留完整数量。
            debugFrame.dart_candidates.assign(dartCandidates.begin(), dartCandidates.begin() +
                std::min<std::size_t>(dartCandidates.size(), 128));
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
            if (yaw.valid) debugFrame.yaw_actual_deg = yaw.degrees;
            if (yaw.seen) debugFrame.yaw_received_epoch_ms = yaw.received_epoch_ms;
            debugFrame.yaw_status = yawStatus;
            debugFrame.processing_fps_5s = webPerformance.processingFps;
            debugFrame.processing_p95_ms = webPerformance.processingP95Ms;
            debugFrame.processing_window_s = webPerformance.windowSeconds;
            debugFrame.processing_window_frames = webPerformance.framesInWindow;
            if (camera) {
                const auto d = camera->diagnostics();
                debugFrame.host_receive_fps = webPerformance.hostFps;
                debugFrame.camera_missing_frames = d.missed;
                debugFrame.camera_gap_events = d.gaps;
            }
            debugFrame.source = webOptions.source;
            debugFrame.source_label = webOptions.source_label;
            debugFrame.mode = webOptions.mode;
            telemetry.publish(debugFrame);
        }

        // M2：录制本帧（逐帧遥测 CSV 直接写；视频帧进入有界队列由后台线程编码落盘）
        {
            webdebug::RecordFrame recordFrame;
            recordFrame.image = annotatedDebug;
            recordFrame.raw_image = image;
            recordFrame.dart_enabled = dartEnabled;
            recordFrame.dart_result = dartResult;
            recordFrame.dart_processing_ms = dartProcessingMs;
            recordFrame.frame_id = frameId;
            recordFrame.timestamp_us = frameTimestampUs;
            recordFrame.detected = target.detected;
            recordFrame.tracking = detector.isTracking();
            recordFrame.target_x = target.center.x;
            recordFrame.target_y = target.center.y;
            recordFrame.radius = target.radius;
            recordFrame.image_width = image.cols;
            recordFrame.image_height = image.rows;
            const Point2f imageCenter = detector.getImageCenter();
            recordFrame.image_center_x = imageCenter.x;
            recordFrame.image_center_y = imageCenter.y;
            recordFrame.offset_x = target.detected ? (target.center.x - imageCenter.x) : 0.0f;
            recordFrame.offset_y = target.detected ? (target.center.y - imageCenter.y) : 0.0f;
            recordFrame.turn_flag = target.turnFlag;
            recordFrame.distance_flag = target.distanceFlag;
            recordFrame.tx_sent = txSent;
            recordFrame.tx_turn_flag = txTurn;
            recordFrame.tx_distance_flag = txDist;
            recordFrame.tx_clear_flag = txClear;
            recordFrame.lock_frames = detector.getConsecutiveFrames();
            recordFrame.required_frames = detector.getRequiredFrames();
            recordFrame.lost_frames = detector.getLostFrames();
            recordFrame.required_lost_frames = detector.getRequiredLostFrames();
            recordFrame.green_pixels = detector.getGreenPixelCount();
            recordFrame.candidate_count = detector.getCandidateCount();
            recordFrame.fps = fpsEma;
            recordFrame.processing_time_ms = processingTimeMs;
            if (yaw.valid) recordFrame.yaw_actual_deg = yaw.degrees;
            if (yaw.seen) recordFrame.yaw_received_epoch_ms = yaw.received_epoch_ms;
            recordFrame.yaw_status = yawStatus;
            recorder.submitFrame(recordFrame);
            if (crossing.valid)
                recorder.appendObservation(referenceLine, crossing, observationTrackId, target.detected,
                                           target.center, image.cols, image.rows);
        }
        perf.add(perf.submit, PerformanceStats::ms(submitStart, PerformanceStats::Clock::now()));

        if (cli.showGui)
        {
            if (target.detected)
            {
                Mat debugImage = annotatedDebug;
                resize(debugImage, resizedImage, displaySize);
                resize(debugImage, displayImage, resultSize, 0, 0, INTER_NEAREST);

                imshow("飞镖制导系统（处理用）", resizedImage);
                imshow("飞镖制导系统（结果展示-超大）", displayImage);
            }
            else
            {
                Mat debugImage = annotatedDebug;
                if (debugImage.empty())
                    debugImage = image;
                resize(debugImage, resizedImage, displaySize);
                resize(debugImage, displayImage, resultSize, 0, 0, INTER_NEAREST);

                imshow("飞镖制导系统（处理用）", resizedImage);
                imshow("飞镖制导系统（结果展示-超大）", displayImage);
            }
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
        perf.add(perf.loop, PerformanceStats::ms(perfLoopStart, PerformanceStats::Clock::now()));
        if (perf.flush()) printCameraStats();
    }

    if (perf.flush(true)) printCameraStats();
    recorder.stop();
    telemetry.stop();
    serial.Close();
    if (cli.showGui)
        destroyAllWindows();

    restoreTerminalOutput();

    return 0;
}

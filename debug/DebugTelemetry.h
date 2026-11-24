/**
 * @file DebugTelemetry.h
 * @brief 视觉调试数据发布层（M1）
 *
 * 设计目标：
 * - 对原有视觉/串口主链路低侵入：视觉线程只做一次互斥锁内的结构体 + Mat 引用计数拷贝，
 *   不做 JPEG 编码、不做网络 IO。
 * - 调试链路故障隔离：Flask 未启动 / 浏览器未连接 / 连接断开时，后台线程丢弃帧并退避重连，
 *   视觉线程永不阻塞，主链路照常运行。
 * - 不引入 UMT / pybind / 嵌入式 Python 等重框架，只依赖 POSIX socket + OpenCV。
 *
 * 数据流：
 *   视觉线程 publish() --> 单槽最新帧（pending_）
 *   后台线程（每 1/publish_fps 秒）取最新帧 --> JPEG + JSON --> TCP(host:port) --> Flask
 *
 * 帧格式（小端）：
 *   magic[4] = "DWDB" | version u8 | flags u8 | reserved u16
 *   | json_len u32 | jpeg_len u32 | json bytes | jpeg bytes
 */

#ifndef WEB_DEBUG_DEBUG_TELEMETRY_H
#define WEB_DEBUG_DEBUG_TELEMETRY_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <opencv2/core.hpp>
#include "tasks/dart/DartTracker.h"

namespace webdebug
{

/// 单帧调试数据快照（全部字段均来自实际算法输出，不做任何估算/伪造）
struct DebugFrame
{
    // 调试图（带识别标注，来自 GreenDetect::getDebugImage()）
    cv::Mat image;
    bool dart_enabled = false;
    std::string dart_color = "red";
    double dart_processing_ms = 0.0;
    uint64_t dart_frame_id = 0;
    int64_t dart_timestamp_us = 0;
    std::optional<double> dart_source_time_ms;
    dartvision::TrackResult dart_result;
    std::vector<dartvision::Candidate> dart_candidates;

    // 识别结果（GreenDetect::TargetInfo）
    bool detected = false;
    float target_x = 0.0f;
    float target_y = 0.0f;
    float radius = 0.0f;
    int image_width = 0;
    int image_height = 0;
    float image_center_x = 0.0f;
    float image_center_y = 0.0f;

    // 原有视觉输出（本帧 GreenDetect 给出的 turnFlag / distanceFlag）
    float turn_flag = 0.0f;
    float distance_flag = 3.0f;

    // 本帧实际通过串口发送的指令（若本帧未发送，tx_sent=false，不伪造数值）
    bool tx_sent = false;
    float tx_turn_flag = 0.0f;
    float tx_distance_flag = 0.0f;
    float tx_clear_flag = 0.0f;

    // 连续帧确认状态（GreenDetect 内部真实状态）
    bool tracking = false;
    int lock_frames = 0;
    int required_frames = 0;
    int lost_frames = 0;
    int required_lost_frames = 0;

    // 预处理/轮廓筛选的真实统计量
    int green_pixels = 0;
    int candidate_count = 0;

    // 主循环实测帧率与识别耗时
    double fps = 0.0;
    double processing_time_ms = 0.0;

    // 数据来源标识
    std::string source;
    std::string source_label;
    std::string mode = "run";
};

/// 发布配置（来自 config/debug.yaml 的 web_debug 段，可被命令行覆盖）
struct Options
{
    bool enabled = true;
    std::string host = "127.0.0.1";
    int port = 5010;
    int publish_fps = 20;
    int jpeg_quality = 75;
    int connect_timeout_ms = 200;
    int send_timeout_ms = 300;
    std::string source = "unknown";
    std::string source_label;
    std::string mode = "run";
};

/**
 * @brief 调试数据发布器
 *
 * 生命周期：构造 -> configure() -> start() -> 视觉循环中 publish() -> stop()/析构
 * start() 失败（如禁用）不会影响后续 publish() 调用（直接返回）。
 */
class DebugTelemetry
{
public:
    DebugTelemetry();
    ~DebugTelemetry();

    DebugTelemetry(const DebugTelemetry &) = delete;
    DebugTelemetry &operator=(const DebugTelemetry &) = delete;

    /// 配置（必须在 start() 前调用）
    void configure(const Options &options);

    /// 启动后台发布线程；options.enabled=false 时直接返回 false
    bool start();

    /// 停止后台线程（幂等）
    void stop();

    /**
     * @brief 发布一帧调试数据（视觉线程调用，非阻塞）
     *
     * 内部只做一次互斥锁内的结构体拷贝与 cv::Mat 引用计数拷贝，微秒级返回。
     * 未启用/未启动时直接丢弃。
     */
    void publish(const DebugFrame &frame);

    bool enabled() const { return options_.enabled; }
    bool started() const { return running_.load(); }
    bool connected() const { return connected_.load(); }
    uint64_t publishedCount() const { return published_count_.load(); }
    uint64_t sentCount() const { return sent_count_.load(); }
    uint64_t droppedCount() const { return dropped_count_.load(); }
    const Options &options() const { return options_; }

private:
    void run();
    bool ensureConnected();
    void closeConnection();
    bool writeAll(const void *data, std::size_t len);
    bool sendFrame(const DebugFrame &frame);
    void logStatsIfDue();

    Options options_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> published_count_{0};
    std::atomic<uint64_t> sent_count_{0};
    std::atomic<uint64_t> dropped_count_{0};

    std::thread worker_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<DebugFrame> pending_;

    int fd_ = -1;

    // 以下仅后台线程访问
    uint64_t seq_ = 0;
    std::chrono::steady_clock::time_point next_send_{};
    std::chrono::steady_clock::time_point next_reconnect_{};
    std::chrono::steady_clock::time_point last_stats_log_{};
};

} // namespace webdebug

#endif // WEB_DEBUG_DEBUG_TELEMETRY_H

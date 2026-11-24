/**
 * @file Recorder.h
 * @brief 视觉数据录制（M2）：会话目录 + 逐帧遥测 CSV + 画面视频 + 视频索引 + 本机控制口
 *
 * 设计目标（对应 taskDART-WEB-M2）：
 * - 录制不依赖 Flask / 浏览器：Flask 未启动、浏览器未打开时照常录制；
 * - 复用队内原有录像流程：仍是 cv::VideoWriter + MJPG，按会话选择原始或识别调试画面，
 *   只是把原来"视觉线程内同步写 record.avi"改为"后台线程 + 有界队列 + 会话目录"；
 * - 逐帧遥测不受预览帧率限制：视觉线程每完成一帧就提交一行 CSV；
 * - 视频索引只记录"确认写入成功"的视频帧，并与算法处理帧号/时间戳关联；
 * - 视觉线程不做磁盘 IO、不做视频缩放（全部在后台线程完成）。
 *
 * 会话目录结构：
 *   recordings/<YYYY-MM-DD_HH-MM-SS>/
 *     ├── session.json                     会话元数据（status: recording -> completed）
 *     ├── config_snapshot/debug.yaml       本次磁盘配置快照
 *     ├── config_snapshot/runtime_params.json  运行时实际生效参数快照
 *     ├── debug_001.avi 或 raw_001.avi    单一来源的视频（MJPG）
 *     ├── telemetry_001.csv               逐帧遥测（每算法帧一行）
 *     └── video_index_001.csv             视频帧 -> 算法帧/时间戳 的逐帧索引
 */

#ifndef WEB_DEBUG_RECORDER_H
#define WEB_DEBUG_RECORDER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include "RuntimeParams.h"
#include "tasks/dart/DartTracker.h"

namespace webdebug
{
class RuntimeParams;

/// 单帧录制数据（由视觉线程提交；全部字段来自本帧实际算法输出）
struct RecordFrame
{
    cv::Mat image;               // 识别调试画面
    cv::Mat raw_image;           // 原始采集画面；进入视频队列时复制
    uint64_t frame_id = 0;       // 视觉处理帧编号（从 1 开始，逐帧递增）
    int64_t timestamp_us = 0;    // 处理完成时刻的 std::chrono::steady_clock 微秒时间戳
    bool detected = false;
    bool tracking = false;
    float target_x = 0.0f;
    float target_y = 0.0f;
    float radius = 0.0f;
    int image_width = 0;
    int image_height = 0;
    float image_center_x = 0.0f;
    float image_center_y = 0.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float turn_flag = 0.0f;
    float distance_flag = 3.0f;
    bool tx_sent = false;        // 本帧是否实际发送串口帧（未发送时 tx_* 不写入 CSV）
    float tx_turn_flag = 0.0f;
    float tx_distance_flag = 0.0f;
    float tx_clear_flag = 0.0f;
    int lock_frames = 0;
    int required_frames = 0;
    int lost_frames = 0;
    int required_lost_frames = 0;
    int green_pixels = 0;
    int candidate_count = 0;
    double fps = 0.0;
    double processing_time_ms = 0.0;
    bool dart_enabled = false;
    dartvision::TrackResult dart_result;
    double dart_processing_ms = 0.0;
};

/// 录制配置（来自 config/debug.yaml 的 recording 段，可被命令行覆盖）
struct RecordOptions
{
    bool enabled = true;             // 录制系统总开关
    bool auto_start = true;          // 程序启动即开始录制
    std::string root = "recordings"; // 会话根目录（相对路径按项目根解析）
    int video_scale = 1;             // 视频缩放：1=调试画面原尺寸；>1 放大（原实现为 3）
    double video_fps = 30.0;         // AVI 标称帧率（仅影响播放器默认速度；真实时间见索引）
    int queue_capacity = 64;         // 视频帧有界队列容量（溢出计为丢帧）
    int telemetry_queue_capacity = 8192;
    int control_port = 5011;         // 本机录制控制口（仅绑 127.0.0.1；0=禁用）
    std::string source;              // 图像来源标识
    std::string source_label;
    std::string mode = "run";
    std::string record_image_mode = "debug";
    std::string config_path;         // config/debug.yaml 路径（用于快照）
    std::string runtime_params_json; // 运行时实际生效参数（JSON 对象字符串）
    std::string app_version;         // 版本/Commit 标识
};

/**
 * @brief 录制器
 *
 * 线程模型：
 * - 视觉线程：submitFrame() 格式化 CSV 并将画面放入有界队列（原始画面会复制）；
 * - telemetry 线程：批量写 telemetry CSV（带缓冲，定期 flush）；
 * - video 线程：缩放 + MJPG 编码 + 写 AVI，成功写入后写 video_index CSV；
 * - control 线程：监听 127.0.0.1 控制口，支持 start/stop/status（本机调试 IPC）。
 */
class Recorder
{
public:
    Recorder();
    ~Recorder();

    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;

    /// 配置（必须在 start() 前调用）
    void configure(const RecordOptions &options);

    /// 启动控制口；options.enabled && auto_start 时立即开始一次录制会话
    void start();

    /// 停止当前会话（若在录制）并关闭控制口（幂等）
    void stop();

    /// 视觉线程每帧调用（非阻塞；未在录制时直接返回）
    void submitFrame(const RecordFrame &frame);

    /// M3：绑定运行时参数服务（控制口 params 命令 + 录制快照）
    void setRuntimeParams(RuntimeParams *params) { runtime_params_ = params; }
    void setDartResetRequest(std::function<void()> callback) { dart_reset_request_ = std::move(callback); }
    void appendDartReset();

    /// M3：参数修改生效时追加一条事件到 events.jsonl（未录制时忽略）
    void appendParameterEvent(const ParamChangeEvent &event);

    /// M4：人工调试记录（机械/电控备注）追加到 events.jsonl；未录制时返回 false
    bool appendManualNote(const std::string &note);

    bool enabled() const { return options_.enabled; }
    uint64_t parameterEvents() const { return parameter_events_.load(); }
    uint64_t manualNotes() const { return manual_notes_.load(); }
    bool isRecording() const { return recording_.load(); }
    uint64_t framesProcessed() const { return frames_processed_.load(); }
    uint64_t telemetryRows() const { return telemetry_rows_.load(); }
    uint64_t videoFrames() const { return video_frames_.load(); }
    uint64_t videoDropped() const { return video_dropped_.load(); }
    uint64_t telemetryDropped() const { return telemetry_dropped_.load(); }

    /// 当前状态 JSON（控制口 / 网页 /api/recorder 使用）
    std::string statusJson() const;

private:
    struct VideoItem
    {
        cv::Mat image;
        uint64_t frame_id = 0;
        int64_t timestamp_us = 0;
        int64_t elapsed_us = 0;
    };

    bool startSession(const std::string &reason, const std::string &image_mode = "debug");
    void stopSession(const std::string &reason);
    void telemetryLoop();
    void videoLoop();
    void controlLoop();
    void writeSessionJson(bool final_write, const std::string &session_status);
    std::string createSessionDir();
    void copyConfigSnapshot();
    void recordError(const std::string &error);

    RecordOptions options_;

    // 状态
    std::atomic<bool> recording_{false};
    std::atomic<bool> stopped_{false};       // stop() 已调用
    std::atomic<bool> control_running_{false};

    // 会话元数据（startSession/stopSession 串行访问，受 session_mutex_ 保护）
    mutable std::mutex session_mutex_;
    std::string session_dir_;
    std::string session_name_;
    std::string session_start_wall_;
    std::string session_end_wall_;
    std::string session_status_;
    std::string record_image_mode_ = "debug";
    int64_t session_start_us_ = 0;
    int64_t session_end_us_ = 0;
    int video_width_ = 0;
    int video_height_ = 0;
    std::string video_error_;

    // CSV 线程
    std::thread telemetry_thread_;
    std::mutex telemetry_mutex_;
    std::condition_variable telemetry_cv_;
    std::deque<std::string> telemetry_queue_;
    bool telemetry_stop_ = false;
    std::ofstream telemetry_file_;
    std::string telemetry_path_;

    // 视频线程
    std::thread video_thread_;
    std::mutex video_mutex_;
    std::condition_variable video_cv_;
    std::deque<VideoItem> video_queue_;
    bool video_stop_ = false;
    cv::VideoWriter video_writer_;
    std::ofstream video_index_file_;
    std::string video_path_;
    std::string video_index_path_;

    // 参数事件（M3）：events.jsonl 追加写入
    RuntimeParams *runtime_params_ = nullptr;
    std::function<void()> dart_reset_request_;
    std::mutex events_mutex_;
    std::ofstream events_file_;
    std::string events_path_;

    // 控制口
    std::thread control_thread_;
    int control_listen_fd_ = -1;

    // 统计
    std::atomic<uint64_t> frames_processed_{0};
    std::atomic<uint64_t> telemetry_rows_{0};
    std::atomic<uint64_t> video_frames_{0};
    std::atomic<uint64_t> video_dropped_{0};
    std::atomic<uint64_t> telemetry_dropped_{0};
    std::atomic<uint64_t> write_errors_{0};
    std::atomic<uint64_t> video_index_rows_{0};
    std::atomic<uint64_t> parameter_events_{0};
    std::atomic<uint64_t> manual_notes_{0};
    // 最近一帧上下文（人工记录事件使用，与参数事件同一时间基准）
    std::atomic<uint64_t> last_frame_id_{0};
    std::atomic<int64_t> last_timestamp_us_{0};

    // 错误列表（受 errors_mutex_ 保护，与 session_mutex_ 分开避免锁嵌套）
    mutable std::mutex errors_mutex_;
    std::vector<std::string> errors_;
};

} // namespace webdebug

#endif // WEB_DEBUG_RECORDER_H

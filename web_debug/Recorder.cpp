/**
 * @file Recorder.cpp
 * @brief 视觉数据录制实现（M2）
 *
 * 锁设计（避免递归锁）：
 * - session_mutex_：会话元数据、文件路径、视频尺寸、状态字符串；
 * - errors_mutex_：错误列表；
 * - telemetry_mutex_ / video_mutex_：各自队列；
 * - writeSessionJson() 内部先做两次短锁快照，再在锁外拼 JSON 并原子替换文件。
 */

#include "Recorder.h"
#include "JsonUtil.h"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace webdebug
{
namespace
{
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

constexpr const char *kTelemetryHeader =
    "frame_id,timestamp_us,elapsed_us,detected,tracking,target_x,target_y,radius,"
    "image_width,image_height,image_center_x,image_center_y,offset_x,offset_y,"
    "turn_flag,distance_flag,clear_flag,tx_sent,tx_turn_flag,tx_distance_flag,tx_clear_flag,"
    "lock_frames,required_frames,lost_frames,required_lost_frames,"
    "green_pixels,candidate_count,fps,processing_time_ms\n";

constexpr const char *kVideoIndexHeader =
    "video_frame_index,source_frame_id,source_timestamp_us,source_elapsed_us\n";

int64_t nowSteadyUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               Clock::now().time_since_epoch())
        .count();
}

std::string nowWallString()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buf[48];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &local);
    return buf;
}

std::string makeTimestampName()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buf[48];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &local);
    return buf;
}

/** 从简单 JSON 对象中提取字符串字段（本机控制口令足够用；不引入 JSON 库） */
std::string extractJsonString(const std::string &json, const std::string &key)
{
    const std::string pattern = "\"" + key + "\"";
    std::size_t pos = json.find(pattern);
    if (pos == std::string::npos)
        return "";
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos)
        return "";
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    if (pos >= json.size() || json[pos] != '"')
        return "";
    ++pos;
    std::string out;
    while (pos < json.size() && json[pos] != '"')
    {
        if (json[pos] == '\\' && pos + 1 < json.size())
        {
            out += json[pos + 1];
            pos += 2;
            continue;
        }
        out += json[pos];
        ++pos;
    }
    return out;
}

bool writeLine(int fd, const std::string &line)
{
    const std::string data = line + "\n";
    std::size_t remaining = data.size();
    const char *p = data.data();
    while (remaining > 0)
    {
        ssize_t n = ::send(fd, p, remaining, MSG_NOSIGNAL);
        if (n > 0)
        {
            p += n;
            remaining -= static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

} // namespace

Recorder::Recorder() = default;

Recorder::~Recorder()
{
    stop();
}

void Recorder::configure(const RecordOptions &options)
{
    options_ = options;
    if (options_.video_scale < 1)
        options_.video_scale = 1;
    if (options_.video_scale > 4)
        options_.video_scale = 4;
    if (options_.video_fps <= 0.0)
        options_.video_fps = 30.0;
    if (options_.queue_capacity < 1)
        options_.queue_capacity = 1;
    if (options_.telemetry_queue_capacity < 16)
        options_.telemetry_queue_capacity = 16;
    session_status_ = "idle";
}

void Recorder::start()
{
    if (!options_.enabled)
    {
        std::cout << "[Recorder] 录制系统未启用（recording.enabled=false）" << std::endl;
        return;
    }

    if (options_.control_port > 0)
    {
        control_running_.store(true);
        control_thread_ = std::thread(&Recorder::controlLoop, this);
        std::cout << "[Recorder] 本机录制控制口: tcp://127.0.0.1:" << options_.control_port
                  << " (status/start/stop)" << std::endl;
    }

    if (options_.auto_start)
        startSession("auto_start");
}

void Recorder::stop()
{
    if (stopped_.exchange(true))
        return;

    // 先停控制口（避免停止过程中又被网页开启新会话），再停当前会话
    control_running_.store(false);
    if (control_listen_fd_ >= 0)
    {
        ::shutdown(control_listen_fd_, SHUT_RDWR);
        ::close(control_listen_fd_);
        control_listen_fd_ = -1;
    }
    if (control_thread_.joinable())
        control_thread_.join();

    if (recording_.load())
        stopSession("program_exit");

    if (options_.enabled)
    {
        std::string dir;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            dir = session_dir_;
        }
        std::cout << "[Recorder] 已停止 (frames=" << frames_processed_.load()
                  << ", telemetry=" << telemetry_rows_.load()
                  << ", video=" << video_frames_.load()
                  << ", video_dropped=" << video_dropped_.load()
                  << ", telemetry_dropped=" << telemetry_dropped_.load()
                  << ", errors=" << write_errors_.load() << ")" << std::endl;
        if (!dir.empty())
            std::cout << "[Recorder] 会话目录: " << dir << std::endl;
    }
}

std::string Recorder::createSessionDir()
{
    fs::path root(options_.root);
    std::error_code ec;
    if (root.is_relative())
        root = fs::absolute(root, ec);
    fs::create_directories(root, ec);

    const std::string base = makeTimestampName();
    fs::path dir = root / base;
    int suffix = 2;
    while (fs::exists(dir, ec))
    {
        dir = root / (base + "_" + std::to_string(suffix));
        ++suffix;
    }
    fs::create_directories(dir, ec);
    if (ec)
        return "";
    return dir.string();
}

void Recorder::copyConfigSnapshot()
{
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        dir = session_dir_;
    }
    if (dir.empty())
        return;

    std::error_code ec;
    const fs::path snapshot_dir = fs::path(dir) / "config_snapshot";
    fs::create_directories(snapshot_dir, ec);

    if (!options_.config_path.empty())
    {
        const fs::path src(options_.config_path);
        if (fs::exists(src, ec))
        {
            fs::copy_file(src, snapshot_dir / "debug.yaml",
                          fs::copy_options::overwrite_existing, ec);
        }
        if (ec)
        {
            recordError("配置文件快照复制失败: " + ec.message());
            ec.clear();
        }
    }

    std::ofstream out(snapshot_dir / "runtime_params.json");
    if (out.is_open())
    {
        out << "{\n"
            << "  \"note\": \"运行时实际生效参数快照（来自主程序变量），与 config_snapshot/debug.yaml "
               "的磁盘配置可能不同\",\n"
            << "  \"params\": " << (options_.runtime_params_json.empty() ? "{}" : options_.runtime_params_json)
            << "\n}\n";
        out.close();
    }
    else
    {
        recordError("runtime_params.json 写入失败");
    }
}

bool Recorder::startSession(const std::string &reason)
{
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (recording_.load())
        {
            std::cout << "[Recorder] 已处于录制状态，忽略 start (" << reason << ")" << std::endl;
            return false;
        }
        if (stopped_.load())
            return false;

        session_dir_ = createSessionDir();
        if (session_dir_.empty())
        {
            recordError("无法创建录制会话目录: " + options_.root);
            std::cerr << "[Recorder] 错误: 无法创建录制会话目录 " << options_.root << std::endl;
            return false;
        }

        session_name_ = fs::path(session_dir_).filename().string();
        session_start_us_ = nowSteadyUs();
        session_start_wall_ = nowWallString();
        session_end_wall_.clear();
        session_end_us_ = 0;
        session_status_ = "recording";
        video_width_ = 0;
        video_height_ = 0;
        video_error_.clear();

        frames_processed_.store(0);
        telemetry_rows_.store(0);
        video_frames_.store(0);
        video_dropped_.store(0);
        telemetry_dropped_.store(0);
        write_errors_.store(0);
        video_index_rows_.store(0);
        {
            std::lock_guard<std::mutex> elock(errors_mutex_);
            errors_.clear();
        }

        // 遥测 CSV / 视频索引 CSV 由各自后台线程独占写入，线程退出时关闭
        telemetry_path_ = (fs::path(session_dir_) / "telemetry_001.csv").string();
        telemetry_file_.open(telemetry_path_, std::ios::out | std::ios::trunc);
        if (telemetry_file_.is_open())
        {
            telemetry_file_ << kTelemetryHeader;
            telemetry_file_.flush();
        }
        else
        {
            recordError("遥测 CSV 打开失败: " + telemetry_path_);
            std::cerr << "[Recorder] 错误: 遥测 CSV 打开失败 " << telemetry_path_ << std::endl;
        }

        video_index_path_ = (fs::path(session_dir_) / "video_index_001.csv").string();
        video_index_file_.open(video_index_path_, std::ios::out | std::ios::trunc);
        if (video_index_file_.is_open())
        {
            video_index_file_ << kVideoIndexHeader;
            video_index_file_.flush();
        }
        else
        {
            recordError("视频索引 CSV 打开失败: " + video_index_path_);
        }

        video_path_ = (fs::path(session_dir_) / "debug_001.avi").string();
    }

    copyConfigSnapshot();

    {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        telemetry_queue_.clear();
        telemetry_stop_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        video_queue_.clear();
        video_stop_ = false;
    }

    // 先启动后台线程，再置录制标志（避免视觉线程推入数据时接收线程尚未启动）
    telemetry_thread_ = std::thread(&Recorder::telemetryLoop, this);
    video_thread_ = std::thread(&Recorder::videoLoop, this);
    recording_.store(true);

    writeSessionJson(false, "recording");

    std::string dir_copy;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        dir_copy = session_dir_;
    }
    std::cout << "[Recorder] 开始录制 (" << reason << "): " << dir_copy << std::endl;
    std::cout << "[Recorder]   视频 debug_001.avi (MJPG, 标称 " << options_.video_fps
              << " fps, scale=" << options_.video_scale << ") | 遥测 telemetry_001.csv | 索引 video_index_001.csv"
              << std::endl;
    return true;
}

void Recorder::stopSession(const std::string &reason)
{
    // 阶段 1：关闭录制入口（视觉线程此后不再提交数据）
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (!recording_.exchange(false))
            return;
    }

    // 阶段 2：通知后台线程并等待其处理完队列
    {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        telemetry_stop_ = true;
    }
    telemetry_cv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        video_stop_ = true;
    }
    video_cv_.notify_all();

    if (telemetry_thread_.joinable())
        telemetry_thread_.join();
    if (video_thread_.joinable())
        video_thread_.join();

    // 阶段 3：收尾状态（注意：writeSessionJson 内部会再次加 session_mutex_，不能在持锁时调用）
    std::string final_status;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_end_us_ = nowSteadyUs();
        session_end_wall_ = nowWallString();
        bool has_errors = false;
        {
            std::lock_guard<std::mutex> elock(errors_mutex_);
            has_errors = !errors_.empty();
        }
        has_errors = has_errors || write_errors_.load() > 0;
        session_status_ = has_errors ? "completed_with_errors" : "completed";
        final_status = session_status_;
    }
    writeSessionJson(true, final_status);

    std::string dir;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        dir = session_dir_;
    }
    std::cout << "[Recorder] 录制停止 (" << reason << "): frames=" << frames_processed_.load()
              << ", telemetry=" << telemetry_rows_.load()
              << ", video=" << video_frames_.load()
              << ", video_dropped=" << video_dropped_.load()
              << ", telemetry_dropped=" << telemetry_dropped_.load()
              << ", errors=" << write_errors_.load() << std::endl;
    std::cout << "[Recorder] 会话已保存: " << dir << std::endl;
}

void Recorder::submitFrame(const RecordFrame &frame)
{
    if (!recording_.load())
        return;

    frames_processed_.fetch_add(1);
    int64_t session_start_us = 0;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_start_us = session_start_us_;
    }
    const int64_t elapsed_us = frame.timestamp_us - session_start_us;

    // ---- 1) 逐帧遥测 CSV（每完成一帧就记录，不受预览帧率限制）----
    char clear_field[32];
    if (frame.tx_sent)
        std::snprintf(clear_field, sizeof(clear_field), "%.2f", frame.tx_clear_flag);
    else
        clear_field[0] = '\0'; // 串口未发送：不伪造 clearFlag

    char line[640];
    const int n = std::snprintf(
        line, sizeof(line),
        "%llu,%lld,%lld,%d,%d,%.2f,%.2f,%.2f,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%s,%d,%.2f,%.2f,%.2f,"
        "%d,%d,%d,%d,%d,%d,%.2f,%.2f\n",
        static_cast<unsigned long long>(frame.frame_id),
        static_cast<long long>(frame.timestamp_us),
        static_cast<long long>(elapsed_us),
        frame.detected ? 1 : 0,
        frame.tracking ? 1 : 0,
        frame.target_x, frame.target_y, frame.radius,
        frame.image_width, frame.image_height,
        frame.image_center_x, frame.image_center_y,
        frame.offset_x, frame.offset_y,
        frame.turn_flag, frame.distance_flag, clear_field,
        frame.tx_sent ? 1 : 0,
        frame.tx_turn_flag, frame.tx_distance_flag, frame.tx_clear_flag,
        frame.lock_frames, frame.required_frames,
        frame.lost_frames, frame.required_lost_frames,
        frame.green_pixels, frame.candidate_count,
        frame.fps, frame.processing_time_ms);
    if (n > 0)
    {
        const int len = std::min<int>(n, static_cast<int>(sizeof(line)) - 1);
        std::string row(line, static_cast<std::size_t>(len));
        {
            std::lock_guard<std::mutex> lk(telemetry_mutex_);
            if (!telemetry_stop_ &&
                telemetry_queue_.size() < static_cast<std::size_t>(options_.telemetry_queue_capacity))
                telemetry_queue_.push_back(std::move(row));
            else
                telemetry_dropped_.fetch_add(1);
        }
        telemetry_cv_.notify_one();
    }

    // ---- 2) 视频帧（有界队列；视觉线程不做缩放/编码/磁盘 IO）----
    if (!frame.image.empty())
    {
        VideoItem item;
        item.image = frame.image; // cv::Mat 引用计数拷贝
        item.frame_id = frame.frame_id;
        item.timestamp_us = frame.timestamp_us;
        item.elapsed_us = elapsed_us;

        bool dropped = false;
        {
            std::lock_guard<std::mutex> lk(video_mutex_);
            if (!video_stop_ && video_queue_.size() < static_cast<std::size_t>(options_.queue_capacity))
                video_queue_.push_back(std::move(item));
            else
                dropped = true;
        }
        if (dropped)
        {
            video_dropped_.fetch_add(1);
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true))
                std::cerr << "[Recorder] 警告: 视频队列已满，开始丢帧（丢帧数会记入 session.json）"
                          << std::endl;
        }
        video_cv_.notify_one();
    }
}

void Recorder::telemetryLoop()
{
    std::vector<std::string> batch;
    batch.reserve(256);
    auto last_flush = Clock::now();

    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(telemetry_mutex_);
            telemetry_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return telemetry_stop_ || !telemetry_queue_.empty();
            });
            if (telemetry_queue_.empty())
            {
                if (telemetry_stop_)
                    break;
                continue;
            }
            const std::size_t take = std::min<std::size_t>(telemetry_queue_.size(), 256);
            batch.clear();
            for (std::size_t i = 0; i < take; ++i)
            {
                batch.push_back(std::move(telemetry_queue_.front()));
                telemetry_queue_.pop_front();
            }
        }

        if (telemetry_file_.is_open())
        {
            for (const auto &row : batch)
                telemetry_file_ << row;
            telemetry_rows_.fetch_add(batch.size());
            if (!telemetry_file_.good())
            {
                write_errors_.fetch_add(1);
                recordError("遥测 CSV 写入失败");
            }
        }

        const auto now = Clock::now();
        if (now - last_flush > std::chrono::seconds(1))
        {
            if (telemetry_file_.is_open())
                telemetry_file_.flush();
            last_flush = now;
        }
    }

    if (telemetry_file_.is_open())
    {
        telemetry_file_.flush();
        telemetry_file_.close();
    }
}

void Recorder::videoLoop()
{
    auto last_session_update = Clock::now();
    uint64_t video_index = 0;

    while (true)
    {
        VideoItem item;
        bool has_item = false;
        {
            std::unique_lock<std::mutex> lock(video_mutex_);
            video_cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return video_stop_ || !video_queue_.empty();
            });
            if (!video_queue_.empty())
            {
                item = std::move(video_queue_.front());
                video_queue_.pop_front();
                has_item = true;
            }
            else if (video_stop_)
            {
                break;
            }
        }

        if (has_item)
        {
            cv::Mat frame = item.image;
            if (options_.video_scale > 1 && !frame.empty())
            {
                cv::Mat scaled;
                cv::resize(frame, scaled, cv::Size(),
                           options_.video_scale, options_.video_scale, cv::INTER_NEAREST);
                frame = scaled;
            }

            if (!frame.empty())
            {
                if (!video_writer_.isOpened())
                {
                    const cv::Size size(frame.cols, frame.rows);
                    const bool ok = video_writer_.open(
                        video_path_, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
                        options_.video_fps, size);
                    if (!ok)
                    {
                        {
                            std::lock_guard<std::mutex> lock(session_mutex_);
                            video_error_ = "VideoWriter 打开失败: " + video_path_;
                        }
                        recordError("VideoWriter 打开失败: " + video_path_);
                        write_errors_.fetch_add(1);
                        std::cerr << "[Recorder] 错误: VideoWriter 打开失败 " << video_path_ << std::endl;
                    }
                    else
                    {
                        {
                            std::lock_guard<std::mutex> lock(session_mutex_);
                            video_width_ = size.width;
                            video_height_ = size.height;
                        }
                        std::cout << "[Recorder] 视频文件已打开: " << video_path_ << " ("
                                  << size.width << "x" << size.height << ")" << std::endl;
                    }
                }

                if (video_writer_.isOpened())
                {
                    video_writer_.write(frame);
                    video_frames_.fetch_add(1);
                    if (video_index_file_.is_open())
                    {
                        video_index_file_ << video_index << ',' << item.frame_id << ','
                                          << item.timestamp_us << ',' << item.elapsed_us << '\n';
                        video_index_rows_.fetch_add(1);
                    }
                    ++video_index;
                }
                else
                {
                    video_dropped_.fetch_add(1);
                }
            }
        }

        const auto now = Clock::now();
        if (now - last_session_update > std::chrono::seconds(5))
        {
            last_session_update = now;
            writeSessionJson(false, "recording");
        }
    }

    if (video_writer_.isOpened())
        video_writer_.release();
    if (video_index_file_.is_open())
    {
        video_index_file_.flush();
        video_index_file_.close();
    }
}

void Recorder::recordError(const std::string &error)
{
    if (error.empty())
        return;
    std::lock_guard<std::mutex> lock(errors_mutex_);
    if (errors_.size() < 50)
        errors_.push_back(error);
}

void Recorder::writeSessionJson(bool final_write, const std::string &session_status)
{
    // 1) 快照元数据（短锁）
    std::string session_dir;
    std::string session_name;
    std::string started_wall;
    std::string ended_wall;
    std::string status;
    std::string video_error;
    int64_t started_us = 0;
    int64_t ended_us = 0;
    int video_width = 0;
    int video_height = 0;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (session_dir_.empty())
            return;
        session_dir = session_dir_;
        session_name = session_name_;
        started_wall = session_start_wall_;
        ended_wall = final_write ? session_end_wall_ : "";
        status = final_write ? (session_status.empty() ? session_status_ : session_status)
                             : (session_status.empty() ? "recording" : session_status);
        video_error = video_error_;
        started_us = session_start_us_;
        ended_us = final_write ? session_end_us_ : nowSteadyUs();
        video_width = video_width_;
        video_height = video_height_;
    }

    // 2) 错误列表快照（另一个短锁）
    std::vector<std::string> errors;
    {
        std::lock_guard<std::mutex> elock(errors_mutex_);
        errors = errors_;
    }

    const double duration_s = started_us > 0 ? static_cast<double>(ended_us - started_us) / 1e6 : 0.0;

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"format_version\": 1,\n";
    oss << "  \"session\": " << jsonQuoted(session_name) << ",\n";
    oss << "  \"status\": " << jsonQuoted(status) << ",\n";
    oss << "  \"started_at_wall\": " << jsonQuoted(started_wall) << ",\n";
    oss << "  \"started_at_monotonic_us\": " << started_us << ",\n";
    oss << "  \"ended_at_wall\": " << jsonQuoted(ended_wall) << ",\n";
    oss << "  \"ended_at_monotonic_us\": " << (final_write ? ended_us : 0) << ",\n";
    oss << "  \"duration_s\": " << jsonNumber(duration_s) << ",\n";
    oss << "  \"time_base\": "
        << jsonQuoted("std::chrono::steady_clock 单调时钟（同一主机/同次开机有效）；"
                      "遥测 CSV 同时保存绝对 timestamp_us 与相对会话的 elapsed_us，回放使用 elapsed_us")
        << ",\n";
    oss << "  \"frame_timestamp_source\": "
        << jsonQuoted("主循环 detectGreenTarget() 处理完成时刻的 steady_clock 时间戳（非相机硬件时间戳）")
        << ",\n";
    oss << "  \"app_version\": " << jsonQuoted(options_.app_version) << ",\n";
    oss << "  \"image_source\": {\"kind\": " << jsonQuoted(options_.source)
        << ", \"label\": " << jsonQuoted(options_.source_label) << "},\n";
    oss << "  \"mode\": " << jsonQuoted(options_.mode) << ",\n";
    oss << "  \"image_size\": {\"width\": " << video_width << ", \"height\": " << video_height << "},\n";
    oss << "  \"counts\": {\n";
    oss << "    \"frames_processed\": " << frames_processed_.load() << ",\n";
    oss << "    \"telemetry_rows\": " << telemetry_rows_.load() << ",\n";
    oss << "    \"video_frames_written\": " << video_frames_.load() << ",\n";
    oss << "    \"video_index_rows\": " << video_index_rows_.load() << ",\n";
    oss << "    \"video_frames_dropped\": " << video_dropped_.load() << ",\n";
    oss << "    \"telemetry_rows_dropped\": " << telemetry_dropped_.load() << ",\n";
    oss << "    \"write_errors\": " << write_errors_.load() << "\n";
    oss << "  },\n";
    oss << "  \"video\": {\n";
    oss << "    \"file\": \"debug_001.avi\",\n";
    oss << "    \"codec\": \"MJPG\",\n";
    oss << "    \"nominal_fps\": " << jsonNumber(options_.video_fps) << ",\n";
    oss << "    \"scale\": " << options_.video_scale << ",\n";
    oss << "    \"width\": " << video_width << ",\n";
    oss << "    \"height\": " << video_height << ",\n";
    oss << "    \"content\": \"GreenDetect::getDebugImage()（含检测标记/目标中心/调试可视化）\",\n";
    oss << "    \"error\": " << jsonQuoted(video_error) << "\n";
    oss << "  },\n";
    oss << "  \"files\": [";
    {
        std::vector<std::string> names;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(session_dir, ec), end; it != end; it.increment(ec))
        {
            if (ec)
                break;
            if (it->is_regular_file())
            {
                std::error_code rel_ec;
                names.push_back(fs::relative(it->path(), session_dir, rel_ec).string());
            }
        }
        std::sort(names.begin(), names.end());
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            if (i > 0)
                oss << ", ";
            oss << jsonQuoted(names[i]);
        }
    }
    oss << "],\n";
    oss << "  \"errors\": [";
    for (std::size_t i = 0; i < errors.size(); ++i)
    {
        if (i > 0)
            oss << ", ";
        oss << jsonQuoted(errors[i]);
    }
    oss << "]\n";
    oss << "}\n";

    // 原子替换，避免回放端读到半截 JSON
    const fs::path final_path = fs::path(session_dir) / "session.json";
    const fs::path tmp_path = fs::path(session_dir) / "session.json.tmp";
    {
        std::ofstream out(tmp_path, std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            std::lock_guard<std::mutex> elock(errors_mutex_);
            if (errors_.size() < 50)
                errors_.push_back("session.json 写入失败");
            return;
        }
        out << oss.str();
        out.close();
    }
    std::error_code ec;
    fs::rename(tmp_path, final_path, ec);
    if (ec)
    {
        std::lock_guard<std::mutex> elock(errors_mutex_);
        if (errors_.size() < 50)
            errors_.push_back("session.json 替换失败: " + ec.message());
    }
}

std::string Recorder::statusJson() const
{
    std::lock_guard<std::mutex> lock(session_mutex_);
    std::ostringstream oss;
    oss << "{";
    oss << "\"ok\":true,";
    oss << "\"enabled\":" << (options_.enabled ? "true" : "false") << ',';
    oss << "\"recording\":" << (recording_.load() ? "true" : "false") << ',';
    oss << "\"session_dir\":" << jsonQuoted(session_dir_) << ',';
    oss << "\"session\":" << jsonQuoted(session_name_) << ',';
    oss << "\"status\":" << jsonQuoted(session_status_) << ',';
    oss << "\"frames_processed\":" << frames_processed_.load() << ',';
    oss << "\"telemetry_rows\":" << telemetry_rows_.load() << ',';
    oss << "\"video_frames\":" << video_frames_.load() << ',';
    oss << "\"video_dropped\":" << video_dropped_.load() << ',';
    oss << "\"telemetry_dropped\":" << telemetry_dropped_.load() << ',';
    oss << "\"write_errors\":" << write_errors_.load() << ',';
    oss << "\"video_width\":" << video_width_ << ',';
    oss << "\"video_height\":" << video_height_;
    oss << "}";
    return oss.str();
}

void Recorder::controlLoop()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        std::cerr << "[Recorder] 控制口 socket 创建失败" << std::endl;
        return;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(options_.control_port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 仅本机可访问，不开放远程写入口
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 4) != 0)
    {
        std::cerr << "[Recorder] 控制口监听失败: 127.0.0.1:" << options_.control_port << std::endl;
        ::close(fd);
        return;
    }
    control_listen_fd_ = fd;

    while (control_running_.load())
    {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int rc = ::poll(&pfd, 1, 200);
        if (rc <= 0)
            continue;

        const int client = ::accept(fd, nullptr, nullptr);
        if (client < 0)
            continue;

        pollfd cfd{};
        cfd.fd = client;
        cfd.events = POLLIN;
        if (::poll(&cfd, 1, 1000) <= 0)
        {
            ::close(client);
            continue;
        }
        char buf[512] = {};
        const ssize_t n = ::recv(client, buf, sizeof(buf) - 1, 0);
        std::string response;
        if (n <= 0)
        {
            response = "{\"ok\":false,\"error\":\"empty request\"}";
        }
        else
        {
            const std::string request(buf, static_cast<std::size_t>(n));
            const std::string cmd = extractJsonString(request, "cmd");
            if (cmd == "status")
            {
                response = statusJson();
            }
            else if (cmd == "start")
            {
                const bool ok = startSession("web_control");
                response = ok ? statusJson()
                              : "{\"ok\":false,\"error\":\"start failed (already recording or dir error)\"}";
            }
            else if (cmd == "stop")
            {
                stopSession("web_control");
                response = statusJson();
            }
            else
            {
                response = "{\"ok\":false,\"error\":\"unknown cmd\"}";
            }
        }
        writeLine(client, response);
        ::close(client);
    }
}

} // namespace webdebug

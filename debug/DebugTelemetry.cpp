/**
 * @file DebugTelemetry.cpp
 * @brief 视觉调试数据发布层实现（M1）
 */

#include "DebugTelemetry.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <vector>

#include <opencv2/imgcodecs.hpp>

namespace webdebug
{
namespace
{
constexpr char kMagic[4] = {'D', 'W', 'D', 'B'};
constexpr uint8_t kProtocolVersion = 1;
constexpr std::size_t kHeaderSize = 16;

using Clock = std::chrono::steady_clock;

/** JSON 字符串转义（保持 UTF-8 原文，仅转义控制字符与引号/反斜杠） */
std::string jsonEscape(const std::string &input)
{
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

void appendFloatField(std::ostringstream &oss, const char *name, double value, bool comma = true)
{
    oss << '"' << name << "\":";
    if (std::isfinite(value))
        oss << value;
    else
        oss << "null";
    if (comma)
        oss << ',';
}

int64_t nowEpochMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void putU32(uint8_t *dst, uint32_t v)
{
    dst[0] = static_cast<uint8_t>(v & 0xFF);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
} // namespace

DebugTelemetry::DebugTelemetry() = default;

DebugTelemetry::~DebugTelemetry()
{
    stop();
}

void DebugTelemetry::configure(const Options &options)
{
    options_ = options;
    // publish_fps: >0 = 主动限速；0 = 不主动限速（每个新帧都发布，跟随算法帧率）
    if (options_.publish_fps < 0)
        options_.publish_fps = 0;
    if (options_.publish_fps > 60)
        options_.publish_fps = 60;
    if (options_.jpeg_quality < 10)
        options_.jpeg_quality = 10;
    if (options_.jpeg_quality > 100)
        options_.jpeg_quality = 100;
}

bool DebugTelemetry::start()
{
    if (running_.load())
        return true;
    if (!options_.enabled)
    {
        std::cout << "[WebDebug] 调试数据接口未启用（web_debug.enabled=false）" << std::endl;
        return false;
    }

    stop_requested_.store(false);
    running_.store(true);
    worker_ = std::thread(&DebugTelemetry::run, this);

    std::cout << "[WebDebug] 调试数据接口已启动: tcp://" << options_.host << ":" << options_.port
              << " (publish_fps="
              << (options_.publish_fps > 0 ? std::to_string(options_.publish_fps) : std::string("0=不限速"))
              << ", jpeg q=" << options_.jpeg_quality << ", source=" << options_.source << ")"
              << std::endl;
    std::cout << "[WebDebug] 无 Flask 连接时自动丢弃调试帧，不影响视觉/串口主链路" << std::endl;
    return true;
}

void DebugTelemetry::stop()
{
    if (!running_.load())
        return;
    stop_requested_.store(true);
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    running_.store(false);
    std::cout << "[WebDebug] 调试数据接口已停止 (published=" << published_count_.load()
              << ", sent=" << sent_count_.load() << ", dropped=" << dropped_count_.load() << ")"
              << std::endl;
}

void DebugTelemetry::publish(const DebugFrame &frame)
{
    if (!running_.load() || !options_.enabled)
        return;

    published_count_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 单槽覆盖：只保留最新帧，绝不排队积压（避免视觉线程被反压）
        pending_ = frame;
    }
    cv_.notify_one();
}

void DebugTelemetry::run()
{
    const auto interval = std::chrono::microseconds(
        options_.publish_fps > 0 ? (1000000 / options_.publish_fps) : 0);
    last_stats_log_ = Clock::now();

    std::vector<uint8_t> jpeg;
    jpeg.reserve(1024 * 256);

    std::optional<DebugFrame> staged; // 已取到、等待发送时隙的帧（最多滞后一个发布周期）

    while (!stop_requested_.load())
    {
        // 1) 若手上没有待发帧，取最新一帧（单槽覆盖，只拿最新）
        if (!staged.has_value())
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return stop_requested_.load() || pending_.has_value();
            });
            if (stop_requested_.load())
                break;
            if (!pending_.has_value())
            {
                // 没有新帧：保持连接可用于检测对端状态，等待下一轮
                logStatsIfDue();
                continue;
            }
            staged = std::move(pending_);
            pending_.reset();
        }

        // 2) 发布节流：没到发布时隙就睡到时隙（期间视觉线程可继续覆盖 pending_）
        const auto now = Clock::now();
        if (now < next_send_)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_until(lock, next_send_, [this]() { return stop_requested_.load(); });
            continue;
        }
        next_send_ = now + interval;

        // 3) 编码并发送（仅在后台线程发生）
        if (!sendFrame(*staged))
            dropped_count_.fetch_add(1);
        staged.reset();
        logStatsIfDue();
    }

    closeConnection();
    running_.store(false);
}

bool DebugTelemetry::ensureConnected()
{
    if (fd_ >= 0)
        return true;

    const auto now = Clock::now();
    if (now < next_reconnect_)
        return false;
    next_reconnect_ = now + std::chrono::seconds(1);

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(options_.port));
    if (::inet_pton(AF_INET, options_.host.c_str(), &addr.sin_addr) != 1)
    {
        std::cerr << "[WebDebug] 无效的 host: " << options_.host << std::endl;
        ::close(fd);
        options_.enabled = false; // 配置错误，停止重试
        return false;
    }

    int rc = ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS)
    {
        ::close(fd);
        return false;
    }

    if (rc != 0)
    {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        const int poll_rc = ::poll(&pfd, 1, options_.connect_timeout_ms);
        if (poll_rc <= 0)
        {
            ::close(fd);
            return false;
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0)
        {
            ::close(fd);
            return false;
        }
    }

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int sndbuf = 4 * 1024 * 1024;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    fd_ = fd;
    connected_.store(true);
    std::cout << "[WebDebug] 已连接调试数据接收端 " << options_.host << ":" << options_.port << std::endl;
    return true;
}

void DebugTelemetry::closeConnection()
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
        if (connected_.exchange(false))
            std::cout << "[WebDebug] 调试数据接收端连接已断开，继续退避重连" << std::endl;
    }
}

bool DebugTelemetry::writeAll(const void *data, std::size_t len)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    std::size_t remaining = len;
    while (remaining > 0)
    {
        ssize_t n = ::send(fd_, p, remaining, MSG_NOSIGNAL);
        if (n > 0)
        {
            p += n;
            remaining -= static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            const int poll_rc = ::poll(&pfd, 1, options_.send_timeout_ms);
            if (poll_rc > 0)
                continue;
            return false;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool DebugTelemetry::sendFrame(const DebugFrame &frame)
{
    if (fd_ < 0 && !ensureConnected())
        return false;

    // 1) JPEG 编码（只在后台线程执行，视觉线程零编码开销）
    std::vector<uint8_t> jpeg;
    jpeg.reserve(256 * 1024);
    if (!frame.image.empty())
    {
        const std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, options_.jpeg_quality};
        if (!cv::imencode(".jpg", frame.image, jpeg, params))
            jpeg.clear();
    }

    // 2) 构建 JSON
    const uint64_t seq = ++seq_;
    std::ostringstream oss;
    oss << '{';
    oss << "\"protocol\":" << static_cast<int>(kProtocolVersion) << ',';
    oss << "\"seq\":" << seq << ',';
    oss << "\"timestamp_ms\":" << nowEpochMs() << ',';
    oss << "\"source\":\"" << jsonEscape(frame.source) << "\",";
    oss << "\"source_label\":\"" << jsonEscape(frame.source_label) << "\",";
    oss << "\"mode\":\"" << jsonEscape(frame.mode) << "\",";
    oss << "\"detected\":" << (frame.detected ? "true" : "false") << ',';
    appendFloatField(oss, "target_x", frame.target_x);
    appendFloatField(oss, "target_y", frame.target_y);
    appendFloatField(oss, "radius", frame.radius);
    oss << "\"image_width\":" << frame.image_width << ',';
    oss << "\"image_height\":" << frame.image_height << ',';
    appendFloatField(oss, "image_center_x", frame.image_center_x);
    appendFloatField(oss, "image_center_y", frame.image_center_y);
    appendFloatField(oss, "offset_x", frame.detected ? (frame.target_x - frame.image_center_x) : 0.0, true);
    appendFloatField(oss, "offset_y", frame.detected ? (frame.target_y - frame.image_center_y) : 0.0, true);
    // 算法输出（每帧都有，即使本帧未发送串口）
    appendFloatField(oss, "turn_flag", frame.turn_flag);
    appendFloatField(oss, "distance_flag", frame.distance_flag);
    // clearFlag 只存在于串口发送帧中：本帧未发送时不给伪造值
    oss << "\"clear_flag\":";
    if (frame.tx_sent)
        oss << frame.tx_clear_flag;
    else
        oss << "null";
    oss << ',';
    oss << "\"tx\":{\"sent\":" << (frame.tx_sent ? "true" : "false");
    if (frame.tx_sent)
    {
        oss << ",\"turn_flag\":" << frame.tx_turn_flag;
        oss << ",\"distance_flag\":" << frame.tx_distance_flag;
        oss << ",\"clear_flag\":" << frame.tx_clear_flag;
    }
    oss << "},";
    appendFloatField(oss, "fps", frame.fps);
    appendFloatField(oss, "processing_time_ms", frame.processing_time_ms);
    oss << "\"tracking\":" << (frame.tracking ? "true" : "false") << ',';
    oss << "\"lock_frames\":" << frame.lock_frames << ',';
    oss << "\"required_frames\":" << frame.required_frames << ',';
    oss << "\"lost_frames\":" << frame.lost_frames << ',';
    oss << "\"required_lost_frames\":" << frame.required_lost_frames << ',';
    oss << "\"green_pixels\":" << frame.green_pixels << ',';
    oss << "\"candidate_count\":" << frame.candidate_count << ',';
    const auto &dart = frame.dart_result;
    oss << "\"dart\":{\"enabled\":" << (frame.dart_enabled ? "true" : "false")
        << ",\"color\":\"" << jsonEscape(frame.dart_color) << "\",\"processing_ms\":" << frame.dart_processing_ms
        << ",\"frame_id\":" << frame.dart_frame_id
        << ",\"timestamp_us\":" << frame.dart_timestamp_us << ",\"source_time_ms\":";
    if (frame.dart_source_time_ms) oss << *frame.dart_source_time_ms; else oss << "null";
    oss << ",\"detected\":" << (dart.detected ? "true" : "false") << ",\"center\":";
    if (dart.center) oss << "{\"x\":" << dart.center->x << ",\"y\":" << dart.center->y << "}";
    else oss << "null";
    oss << ",\"bbox\":";
    if (dart.bbox) oss << "{\"x\":" << dart.bbox->x << ",\"y\":" << dart.bbox->y
                       << ",\"width\":" << dart.bbox->width << ",\"height\":" << dart.bbox->height << "}";
    else oss << "null";
    oss << ",\"candidate_count\":" << dart.candidate_count
        << ",\"track_valid\":" << (dart.track_valid ? "true" : "false")
        << ",\"lost_frames\":" << dart.lost_frames
        << ",\"track_state\":\"" << jsonEscape(dart.track_state)
        << "\",\"failure_reason\":\"" << jsonEscape(dart.failure_reason)
        << "\",\"candidates_truncated\":"
        << (dart.candidate_count > static_cast<int>(frame.dart_candidates.size()) ? "true" : "false")
        << ",\"candidates\":[";
    for (std::size_t i = 0; i < frame.dart_candidates.size(); ++i) {
        const auto &c = frame.dart_candidates[i];
        if (i) oss << ',';
        oss << "{\"x\":" << c.bbox.x << ",\"y\":" << c.bbox.y
            << ",\"width\":" << c.bbox.width << ",\"height\":" << c.bbox.height
            << ",\"area\":" << c.area << "}";
    }
    oss << "],\"track_points\":[";
    for (std::size_t i = 0; i < dart.track_points.size(); ++i) {
        const auto &p = dart.track_points[i];
        if (i) oss << ',';
        oss << "{\"frame_id\":" << p.frame_id << ",\"timestamp_us\":" << p.timestamp_us
            << ",\"source_time_ms\":";
        if (p.source_time_ms) oss << *p.source_time_ms; else oss << "null";
        oss << ",\"x\":" << p.center.x << ",\"y\":" << p.center.y << "}";
    }
    oss << "]},";
    oss << "\"jpeg_bytes\":" << jpeg.size();
    oss << '}';
    const std::string json = oss.str();

    // 3) 组包：16 字节头 + JSON + JPEG
    uint8_t header[kHeaderSize] = {};
    std::memcpy(header, kMagic, 4);
    header[4] = kProtocolVersion;
    header[5] = 0; // flags
    header[6] = 0;
    header[7] = 0;
    putU32(header + 8, static_cast<uint32_t>(json.size()));
    putU32(header + 12, static_cast<uint32_t>(jpeg.size()));

    if (!writeAll(header, kHeaderSize) ||
        !writeAll(json.data(), json.size()) ||
        (!jpeg.empty() && !writeAll(jpeg.data(), jpeg.size())))
    {
        closeConnection();
        return false;
    }

    sent_count_.fetch_add(1);
    return true;
}

void DebugTelemetry::logStatsIfDue()
{
    const auto now = Clock::now();
    if (now - last_stats_log_ < std::chrono::seconds(10))
        return;
    last_stats_log_ = now;
    std::cout << "[WebDebug] published=" << published_count_.load()
              << " sent=" << sent_count_.load()
              << " dropped=" << dropped_count_.load()
              << " connected=" << (connected_.load() ? "yes" : "no") << std::endl;
}

} // namespace webdebug

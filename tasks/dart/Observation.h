#pragma once
#include "DartTracker.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

namespace dartvision {
// 仅模拟事件控制，不解析串口，也不声明真实发射发生。
struct WindowConfig {
    bool enabled = false;
    int64_t delay_ms = 0, duration_ms = 1000, max_tracking_ms = 2000;
};
class FlightWindow {
public:
    explicit FlightWindow(WindowConfig config = {}) : config_(config) {}
    void reset() { event_id_.reset(); established_ms_.reset(); seen_events_.clear(); }
    void configure(WindowConfig config) {
        if (config.enabled != config_.enabled || config.delay_ms != config_.delay_ms ||
            config.duration_ms != config_.duration_ms || config.max_tracking_ms != config_.max_tracking_ms) {
            reset(); config_ = config;
        }
    }
    bool trigger(uint64_t event_id, int64_t now_ms) {
        if (!seen_events_.insert(event_id).second) return false;
        event_id_ = event_id; event_ms_ = now_ms; established_ms_.reset(); return true;
    }
    bool allowsNew(int64_t now_ms) const {
        if (!config_.enabled) return true;
        return event_id_ && now_ms >= event_ms_ + config_.delay_ms &&
               now_ms < event_ms_ + config_.delay_ms + config_.duration_ms;
    }
    void established(int64_t now_ms) { if (!established_ms_) established_ms_ = now_ms; }
    bool allowsExisting(int64_t now_ms) const {
        return !config_.enabled || (established_ms_ && now_ms >= *established_ms_ &&
                                  now_ms - *established_ms_ <= config_.max_tracking_ms);
    }
private:
    WindowConfig config_;
    std::unordered_set<uint64_t> seen_events_;
    std::optional<uint64_t> event_id_;
    std::optional<int64_t> established_ms_;
    int64_t event_ms_ = 0;
};

// 参考线为图像坐标下有限线段；结果是检测框中心轨迹观测，不是镖尖或落点。
struct ReferenceLine { std::string id; cv::Point2d a, b; };
struct Crossing {
    bool valid = false;
    std::string reason = "not_crossed", line_id;
    cv::Point2d point;
    uint64_t before_frame = 0, after_frame = 0;
    int64_t before_timestamp_us = 0, after_timestamp_us = 0;
    std::optional<double> interpolated_source_ms;
};
inline double cross2(cv::Point2d a, cv::Point2d b) { return a.x*b.y-a.y*b.x; }
inline Crossing measureCrossing(const ReferenceLine &line, const TrackPoint &previous, const TrackPoint &current) {
    Crossing out; out.line_id = line.id;
    if (line.id.empty() || !std::isfinite(line.a.x) || !std::isfinite(line.a.y) ||
        !std::isfinite(line.b.x) || !std::isfinite(line.b.y) || cv::norm(line.b-line.a) < 1e-9) {
        out.reason="invalid_line"; return out;
    }
    if (current.frame_id != previous.frame_id + 1 || current.timestamp_us < previous.timestamp_us) {
        out.reason="frame_gap_or_time_invalid"; return out;
    }
    const cv::Point2d p(previous.center), r=cv::Point2d(current.center)-p;
    const cv::Point2d q=line.a, s=line.b-line.a;
    const double denom=cross2(r,s);
    if (std::abs(denom)<1e-9) { out.reason="parallel_or_stationary"; return out; }
    const double t=cross2(q-p,s)/denom, u=cross2(q-p,r)/denom;
    // 排除起点恰在线上的重复计数；前一段已记录该端点。
    if (t<=1e-9 || t>1 || u<0 || u>1) return out;
    out.valid=true;out.reason.clear();out.point=p+t*r;
    out.before_frame=previous.frame_id;out.after_frame=current.frame_id;
    out.before_timestamp_us=previous.timestamp_us;out.after_timestamp_us=current.timestamp_us;
    if (previous.source_time_ms && current.source_time_ms && *current.source_time_ms >= *previous.source_time_ms)
        out.interpolated_source_ms=*previous.source_time_ms+t*(*current.source_time_ms-*previous.source_time_ms);
    return out;
}
}

#include "DartTracker.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace dartvision {
void DartTracker::reset() {
    result_ = TrackResult{}; previous_.clear(); previous_frame_ = 0;
    previous_timestamp_ = 0; previous_source_time_.reset(); velocity_ = {0, 0};
}
TrackResult DartTracker::update(const std::vector<Candidate> &candidates, uint64_t frame_id,
                                int64_t timestamp_us, std::optional<double> source_time_ms) {
    result_.candidate_count = static_cast<int>(candidates.size());
    result_.detected = false; result_.center.reset(); result_.bbox.reset();
    const Candidate *selected = nullptr;
    if (!result_.track_valid) {
        // 只凭面积不启动：两帧中匹配到有位移的候选才建立轨迹，过滤静止同色物体。
        double best = std::numeric_limits<double>::max();
        const Candidate *origin = nullptr;
        if (previous_frame_ + 1 == frame_id) for (const auto &now : candidates) for (const auto &prev : previous_) {
            const double distance = cv::norm(now.center - prev.center);
            if (distance < config_.min_motion || distance > config_.max_distance) continue;
            if (distance < best) { best = distance; selected = &now; origin = &prev; }
        }
        if (selected) {
            result_.track_points.clear();
            result_.track_points.push_back({previous_frame_, previous_timestamp_, previous_source_time_, origin->center});
            velocity_ = selected->center - origin->center;
            result_.track_valid = true; result_.lost_frames = 0;
            result_.track_state = "captured"; result_.failure_reason.clear();
        } else {
            if (result_.track_state != "failed") {
                result_.track_state = "waiting";
                result_.failure_reason = candidates.empty() ? "no_detection" : "insufficient_motion";
            }
        }
    } else {
        const auto last = result_.track_points.back();
        const float steps = static_cast<float>(std::max<uint64_t>(1, frame_id - last.frame_id));
        const cv::Point2f predicted = last.center + velocity_ * steps;
        double best = std::numeric_limits<double>::max();
        for (const auto &candidate : candidates) {
            const cv::Point2f displacement = candidate.center - last.center;
            const double distance = cv::norm(displacement);
            if (distance > config_.max_distance * steps) continue;
            const double vnorm = cv::norm(velocity_);
            const double dnorm = cv::norm(displacement);
            if (vnorm >= config_.min_motion && dnorm >= config_.min_motion &&
                velocity_.dot(displacement) / (vnorm * dnorm) < -0.3) continue;
            const double score = cv::norm(candidate.center - predicted);
            if (score < best) { best = score; selected = &candidate; }
        }
        if (selected) {
            velocity_ = (selected->center - last.center) * (1.0f / steps);
            result_.track_state = result_.lost_frames ? "recaptured" : "tracking";
            result_.lost_frames = 0; result_.failure_reason.clear();
        } else {
            ++result_.lost_frames;
            result_.failure_reason = candidates.empty() ? "no_detection" : "association_failed";
            if (result_.lost_frames > std::max(0, config_.max_lost_frames)) {
                result_.track_valid = false; result_.track_state = "failed";
                result_.failure_reason = "track_timeout";
            } else result_.track_state = "lost";
        }
    }
    if (selected) {
        result_.detected = true; result_.center = selected->center; result_.bbox = selected->bbox;
        result_.track_points.push_back({frame_id, timestamp_us, source_time_ms, selected->center});
        const auto max_points = static_cast<std::size_t>(std::max(2, config_.max_points));
        if (result_.track_points.size() > max_points)
            result_.track_points.erase(result_.track_points.begin(), result_.track_points.end() - max_points);
    }
    previous_ = candidates; previous_frame_ = frame_id;
    previous_timestamp_ = timestamp_us; previous_source_time_ = source_time_ms;
    return result_;
}
}

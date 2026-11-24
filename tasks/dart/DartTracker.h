#pragma once
#include "DartDetector.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dartvision {
struct TrackerConfig {
    double max_distance = 120.0;
    double min_motion = 4.0;
    int max_lost_frames = 5;
    int max_points = 100;
};
struct TrackPoint {
    uint64_t frame_id = 0;
    int64_t timestamp_us = 0; // 进程 steady_clock，非飞行时间
    std::optional<double> source_time_ms; // 视频时间戳，仅源可提供时填写
    cv::Point2f center;
};
struct TrackResult {
    bool detected = false;
    std::optional<cv::Point2f> center;
    std::optional<cv::Rect> bbox;
    int candidate_count = 0;
    bool track_valid = false;
    std::vector<TrackPoint> track_points;
    int lost_frames = 0;
    std::string track_state = "waiting";
    std::string failure_reason = "no_detection";
};
class DartTracker {
public:
    explicit DartTracker(TrackerConfig config = {}) : config_(std::move(config)) {}
    TrackResult update(const std::vector<Candidate> &candidates, uint64_t frame_id,
                       int64_t timestamp_us, std::optional<double> source_time_ms = {});
    void reset();
private:
    TrackerConfig config_;
    TrackResult result_;
    std::vector<Candidate> previous_;
    uint64_t previous_frame_ = 0;
    int64_t previous_timestamp_ = 0;
    std::optional<double> previous_source_time_;
    cv::Point2f velocity_{0, 0};
};
}

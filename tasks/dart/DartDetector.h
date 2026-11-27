#pragma once
#include <opencv2/core.hpp>
#include <string>
#include <utility>
#include <vector>

namespace dartvision {
struct DetectorConfig {
    std::string color = "red";
    int saturation_min = 100, value_min = 80;
    int red_low_max = 10, red_high_min = 170;
    int blue_hue_min = 90, blue_hue_max = 135;
    double min_area = 20, max_area = 5000;
    double min_aspect = 0.15, max_aspect = 8.0;
    int close_kernel = 3;
    bool roi_enabled = false;
    cv::Rect roi;
    bool circularity_filter_enabled = false;
    double max_circularity = 0.8; // 可选，须按实拍标定后启用
};
struct DetectionDiagnostics {
    bool invalid_roi = false;
    unsigned circularity_rejected = 0;
};
struct Candidate {
    cv::Point2f center;
    cv::Rect bbox;
    double area = 0;
};
class DartDetector {
public:
    explicit DartDetector(DetectorConfig config = {}) : config_(std::move(config)) {}
    std::vector<Candidate> detect(const cv::Mat &image, DetectionDiagnostics *diagnostics = nullptr) const;
private:
    DetectorConfig config_;
};
}

#include "DartDetector.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>

namespace dartvision {
std::vector<Candidate> DartDetector::detect(const cv::Mat &image, DetectionDiagnostics *diagnostics) const {
    if (diagnostics) *diagnostics = {};
    std::vector<Candidate> result;
    if (image.empty() || image.type() != CV_8UC3) return result;
    cv::Rect roi(0, 0, image.cols, image.rows);
    if (config_.roi_enabled) {
        const cv::Rect r = config_.roi;
        const bool valid = r.x >= 0 && r.y >= 0 && r.width > 0 && r.height > 0 &&
                           r.x < image.cols && r.y < image.rows &&
                           r.width <= image.cols - r.x && r.height <= image.rows - r.y;
        if (!valid) {
            if (diagnostics) diagnostics->invalid_roi = true;
            return result; // 无效 ROI 不静默回退成可能误检的全图
        }
        roi = r;
    }
    cv::Mat hsv, mask;
    cv::cvtColor(image(roi), hsv, cv::COLOR_BGR2HSV);
    if (config_.color == "red") {
        cv::Mat high;
        cv::inRange(hsv, cv::Scalar(0, config_.saturation_min, config_.value_min),
                    cv::Scalar(config_.red_low_max, 255, 255), mask);
        cv::inRange(hsv, cv::Scalar(config_.red_high_min, config_.saturation_min, config_.value_min),
                    cv::Scalar(179, 255, 255), high);
        cv::bitwise_or(mask, high, mask);
    } else if (config_.color == "blue") {
        cv::inRange(hsv, cv::Scalar(config_.blue_hue_min, config_.saturation_min, config_.value_min),
                    cv::Scalar(config_.blue_hue_max, 255, 255), mask);
    } else return result;
    const int k = std::clamp(config_.close_kernel | 1, 1, 31);
    if (k > 1) cv::morphologyEx(mask, mask, cv::MORPH_CLOSE,
        cv::getStructuringElement(cv::MORPH_RECT, cv::Size(k, k)));
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    for (const auto &contour : contours) {
        const double area = cv::contourArea(contour);
        const cv::Rect box = cv::boundingRect(contour);
        const double aspect = box.height ? static_cast<double>(box.width) / box.height : 0;
        if (area < config_.min_area || area > config_.max_area ||
            aspect < config_.min_aspect || aspect > config_.max_aspect) continue;
        if (config_.circularity_filter_enabled) {
            const double perimeter = cv::arcLength(contour, true);
            // 面积/周长退化时不作圆形判定，避免除零和将碎片当作圆。
            if (area > 0.0 && perimeter > 1.0) {
                const double circularity = 4.0 * CV_PI * area / (perimeter * perimeter);
                if (circularity >= config_.max_circularity) {
                    if (diagnostics) ++diagnostics->circularity_rejected;
                    continue;
                }
            }
        }
        cv::Rect fullBox(box.x + roi.x, box.y + roi.y, box.width, box.height);
        result.push_back({cv::Point2f(fullBox.x + fullBox.width * 0.5f, fullBox.y + fullBox.height * 0.5f), fullBox, area});
    }
    return result;
}
}

#include "DartDetector.h"
#include <opencv2/imgproc.hpp>
#include <algorithm>

namespace dartvision {
std::vector<Candidate> DartDetector::detect(const cv::Mat &image) const {
    std::vector<Candidate> result;
    if (image.empty() || image.type() != CV_8UC3) return result;
    cv::Mat hsv, mask;
    cv::cvtColor(image, hsv, cv::COLOR_BGR2HSV);
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
        result.push_back({cv::Point2f(box.x + box.width * 0.5f, box.y + box.height * 0.5f), box, area});
    }
    return result;
}
}

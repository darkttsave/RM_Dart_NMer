#include "tasks/dart/GreenDetect.h"
#include <opencv2/opencv.hpp>
#include <iostream>

int main()
{
    GreenDetect reference, candidate;
    reference.setRequiredFrames(3); candidate.setRequiredFrames(3);
    reference.setRequiredLostFrames(3); candidate.setRequiredLostFrames(3);
    for (int i = 0; i < 80; ++i) {
        const bool render = i >= 35 && i < 55;
        candidate.setDebugRendering(render);
        cv::Mat image(360, 640, CV_8UC3, cv::Scalar(5, 5, 5));
        if (i % 12 < 8) cv::circle(image, cv::Point(210 + i % 30, 180), 18, cv::Scalar(5, 255, 5), -1);
        const auto a = reference.detectGreenTarget(image);
        const auto b = candidate.detectGreenTarget(image);
        if (a.detected != b.detected || a.center != b.center || a.radius != b.radius ||
            a.turnFlag != b.turnFlag || a.distanceFlag != b.distanceFlag || a.boundingBox != b.boundingBox ||
            reference.isTracking() != candidate.isTracking() ||
            reference.getConsecutiveFrames() != candidate.getConsecutiveFrames() ||
            reference.getLostFrames() != candidate.getLostFrames() ||
            reference.getGreenPixelCount() != candidate.getGreenPixelCount() ||
            reference.getCandidateCount() != candidate.getCandidateCount()) return 1;
        if (render) {
            if (candidate.getDebugImage().empty() || candidate.getBinaryImage().empty() ||
                cv::norm(reference.getDebugImage(), candidate.getDebugImage(), cv::NORM_INF) != 0 ||
                cv::norm(reference.getBinaryImage(), candidate.getBinaryImage(), cv::NORM_INF) != 0) return 2;
        } else if (!candidate.getDebugImage().empty() || !candidate.getBinaryImage().empty()) return 3;
    }
    GreenDetect disappearance;
    disappearance.setRequiredFrames(1);
    disappearance.setRequiredLostFrames(2);
    cv::Mat target(360, 640, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::circle(target, cv::Point(320, 180), 18, cv::Scalar(0, 255, 0), -1);
    if (!disappearance.detectGreenTarget(target).detected || !disappearance.isTracking()) return 4;
    cv::Mat empty = cv::Mat::zeros(target.size(), target.type());
    const auto lost = disappearance.detectGreenTarget(empty);
    if (lost.detected || !disappearance.isTracking() || disappearance.getLostFrames() != 1 ||
        disappearance.getGreenPixelCount() != 0 || disappearance.getCandidateCount() != 0 ||
        disappearance.getBinaryImage().empty() || cv::countNonZero(disappearance.getBinaryImage()) != 0 ||
        disappearance.getDebugImage().empty()) return 5;
    disappearance.detectGreenTarget(empty);
    if (disappearance.isTracking() || disappearance.getLostFrames() != 2) return 6;
    std::cout << "PASS 调试绘制切换、空掩码及目标丢失状态\n";
}

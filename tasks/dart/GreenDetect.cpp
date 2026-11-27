/**
 * @file GreenDetect.cpp
 * @brief 绿灯识别实现
 * @author lxy
 * @date 2025-10-24
 */

#include "GreenDetect.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <yaml-cpp/yaml.h>

namespace {
using Clock = std::chrono::steady_clock;
struct StageTimer {
    bool enabled; double &result; Clock::time_point start;
    StageTimer(bool on, double &target) : enabled(on), result(target) { if (enabled) start = Clock::now(); }
    ~StageTimer() { if (enabled) result += std::chrono::duration<double, std::milli>(Clock::now()-start).count(); }
};
}

GreenDetect::ProfileFrame::ProfileFrame(GreenDetect &o) : owner(o) { if (owner.profileEnabled_) owner.profileTimes_.fill(0); }
GreenDetect::ProfileFrame::~ProfileFrame() { if (owner.profileEnabled_) owner.finishProfileFrame(); }
void GreenDetect::finishProfileFrame()
{
    for (size_t i=0; i<profileTimes_.size(); ++i) profileSamples_[i].push_back(profileTimes_[i]);
    if (Clock::now() - profileStart_ < std::chrono::seconds(5)) return;
    const char *names[] = {"G01_hsv_brightness", "G02_split", "G03_fused_color", "G04_dominance", "G05_morphology", "G06_contours", "G07_geometry", "G08_drawing"};
    for (size_t i=0; i<profileSamples_.size(); ++i) {
        auto &v = profileSamples_[i];
        const double mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
        std::sort(v.begin(), v.end());
        std::cout << "[GreenPerf] " << names[i] << " n=" << v.size() << " mean_ms=" << mean
                  << " p50_ms=" << v[(v.size()-1)/2] << " p95_ms=" << v[(v.size()-1)*95/100]
                  << " max_ms=" << v.back() << std::endl;
        v.clear();
    }
    profileStart_ = Clock::now();
}

GreenDetect::GreenDetect()
    : minRadius_(0), maxRadius_(50), imageCenter_(720, 540),
      turnThreshold_(5.0f), brightThresh_(80), greenWeightThresh_(0.4f),
      consecutiveFrames_(0), requiredFrames_(10),
      lostFrames_(0), requiredLostFrames_(10), tracking_(false),
      lastCenter_(0, 0), maxCenterDist_(100.0f),
      kernelSize_(5), openIterations_(1), closeIterations_(1),
      distFarThresh_(200.0f), distNearThresh_(30.0f),
      minCircularity_(0.6), minAspectRatio_(0.5), maxAspectRatio_(2.0),
      minArea_(50), maxArea_(50000)
{
    // 尝试从YAML配置文件加载检测参数
    try
    {
        const std::vector<std::string> candidates = {
            "config/debug.yaml",
            "./config/debug.yaml",
            "../config/debug.yaml"};
        YAML::Node config;
        bool loaded = false;
        for (const auto &p : candidates)
        {
            try
            {
                config = YAML::LoadFile(p);
                loaded = true;
                break;
            }
            catch (...)
            {
            }
        }
        if (!loaded)
            throw std::runtime_error("config not found");

        minRadius_ = config["detection"]["min_radius"].as<int>();
        maxRadius_ = config["detection"]["max_radius"].as<int>();
        turnThreshold_ = config["turn_control"]["turn_threshold"].as<float>();

        if (config["detection"]["bright_thresh"])
            brightThresh_ = config["detection"]["bright_thresh"].as<int>();
        if (config["detection"]["green_weight_thresh"])
            greenWeightThresh_ = config["detection"]["green_weight_thresh"].as<float>();

        // 加载时间滤波参数（如果存在）
        if (config["detection"]["time_filter"])
        {
            auto tf = config["detection"]["time_filter"];
            requiredFrames_ = tf["required_frames"].as<int>();
            maxCenterDist_ = tf["max_center_dist"].as<float>();
            if (tf["required_lost_frames"])
                requiredLostFrames_ = tf["required_lost_frames"].as<int>();
        }

        // 加载形态学参数（如果存在）
        if (config["detection"]["morphology"])
        {
            kernelSize_ = config["detection"]["morphology"]["kernel_size"].as<int>();
            openIterations_ = config["detection"]["morphology"]["open_iterations"].as<int>();
            closeIterations_ = config["detection"]["morphology"]["close_iterations"].as<int>();
        }

        // 加载距离控制阈值
        if (config["turn_control"]["dist_far_thresh"])
            distFarThresh_ = config["turn_control"]["dist_far_thresh"].as<float>();
        if (config["turn_control"]["dist_near_thresh"])
            distNearThresh_ = config["turn_control"]["dist_near_thresh"].as<float>();

        if (config["detection"]["shape_filter"])
        {
            auto sf = config["detection"]["shape_filter"];
            if (sf["min_circularity"])
                minCircularity_ = sf["min_circularity"].as<double>();
            if (sf["min_aspect_ratio"])
                minAspectRatio_ = sf["min_aspect_ratio"].as<double>();
            if (sf["max_aspect_ratio"])
                maxAspectRatio_ = sf["max_aspect_ratio"].as<double>();
            if (sf["min_area"])
                minArea_ = sf["min_area"].as<double>();
            if (sf["max_area"])
                maxArea_ = sf["max_area"].as<double>();
        }

        std::cout << "[GreenDetect] Config loaded - turnThreshold: " << turnThreshold_ << std::endl;
        std::cout << "[GreenDetect] Bright thresh: " << brightThresh_ << ", Green weight thresh: " << greenWeightThresh_ << std::endl;
        std::cout << "[GreenDetect] Morphology: kernel=" << kernelSize_ << " open=" << openIterations_ << " close=" << closeIterations_ << std::endl;
    }
    catch (const std::exception &e)
    {
        std::cerr << "[GreenDetect] Warning: Failed to load config file, using default values: " << e.what() << std::endl;
    }
}

GreenDetect::~GreenDetect()
{
}

GreenDetect::TargetInfo GreenDetect::detectGreenTarget(const Mat &image)
{
    ProfileFrame profile(*this);
    TargetInfo result;
    result.detected = false;
    result.turnFlag = 0.0f;
    result.distanceFlag = 3.0f; // 默认：对准/无目标
    result.center = Point2f(0, 0);
    result.radius = 0;
    result.boundingBox = Rect(0, 0, 0, 0);
    greenPixelCount_ = 0;
    candidateCount_ = 0;

    if (image.empty() || image.channels() != 3 || image.depth() != CV_8U ||
        image.cols < 100 || image.rows < 100)
        return result;

    imageCenter_ = Point2f(image.cols / 2.0f, image.rows / 2.0f);
    if (drawDebug_) { StageTimer t(profileEnabled_, profileTimes_[7]); debugImage_ = image.clone(); }
    else debugImage_.release();

    roiInvalid_ = false;
    Mat greenBinary;
    if (roiEnabled_)
    {
        const auto &r = roi_;
        const bool valid = r.x >= 0 && r.y >= 0 && r.width > 0 && r.height > 0 &&
                           r.x < image.cols && r.y < image.rows &&
                           r.width <= image.cols - r.x && r.height <= image.rows - r.y;
        if (!valid)
        {
            // 安全失败：不输出误导性的目标或改变图像中心。
            roiInvalid_ = true;
            consecutiveFrames_ = 0;
            tracking_ = false;
            lostFrames_ = 0;
            binaryImage_.release();
            if (drawDebug_) cv::putText(debugImage_, "Invalid green ROI", Point(10, 30),
                                       cv::FONT_HERSHEY_SIMPLEX, 0.8, Scalar(0, 0, 255), 2);
            return result;
        }
        Mat cropped = preprocessGreenChannel(image(r));
        greenBinary = Mat::zeros(image.size(), CV_8UC1);
        cropped.copyTo(greenBinary(r));
        if (drawDebug_) binaryImage_ = greenBinary.clone();
    }
    else greenBinary = preprocessGreenChannel(image);

    // 判断视野内是否有绿色像素
    int greenPixels = cv::countNonZero(greenBinary);
    greenPixelCount_ = greenPixels;
    if (greenPixels == 0)
    {
        result.distanceFlag = 3.0f;
        consecutiveFrames_ = 0;
        if (tracking_)
        {
            lostFrames_++;
            if (lostFrames_ >= requiredLostFrames_)
                tracking_ = false;
        }
        if (drawDebug_) {
            StageTimer t(profileEnabled_, profileTimes_[7]);
            cv::putText(debugImage_, "No Green In View",
                        Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 1, Scalar(0, 0, 255), 2);
        }
        return result;
    }

    const auto geometryStart = profileEnabled_ ? Clock::now() : Clock::time_point{};
    const double contourBefore = profileTimes_[5], drawingBefore = profileTimes_[7];
    std::vector<Vec3f> circles = detectCircles(greenBinary);
    if (profileEnabled_) profileTimes_[6] += std::max(0.0, std::chrono::duration<double, std::milli>(Clock::now()-geometryStart).count() - (profileTimes_[5]-contourBefore) - (profileTimes_[7]-drawingBefore));

    if (!circles.empty())
    {
        Vec3f circ = circles[0];
        Point2f currentCenter = Point2f(circ[0], circ[1]);

        if (consecutiveFrames_ > 0)
        {
            float dist = cv::norm(currentCenter - lastCenter_);
            if (dist < maxCenterDist_)
                consecutiveFrames_++;
            else
                consecutiveFrames_ = 1;
        }
        else
        {
            consecutiveFrames_ = 1;
        }
        lastCenter_ = currentCenter;
        lostFrames_ = 0;

        if (!tracking_ && consecutiveFrames_ >= requiredFrames_)
            tracking_ = true;

        if (tracking_)
        {
            result.detected = true;
            result.center = currentCenter;
            result.radius = circ[2];
            result.boundingBox = Rect(
                static_cast<int>(result.center.x - result.radius),
                static_cast<int>(result.center.y - result.radius),
                static_cast<int>(result.radius * 2),
                static_cast<int>(result.radius * 2));

            calculateTurnDirection(result.center, result);

            // 三段式距离控制：用中心偏移量（像素）估算距离阶段
            float offset = std::abs(result.center.x - imageCenter_.x);
            if (offset > distFarThresh_)
                result.distanceFlag = 1.0f;
            else if (offset > distNearThresh_)
                result.distanceFlag = 2.0f;
            else
                result.distanceFlag = 3.0f;
        }

        if (drawDebug_) {
            StageTimer t(profileEnabled_, profileTimes_[7]);
            cv::circle(debugImage_, result.center, result.radius, Scalar(0, 255, 255), 4);
            int crossSize = std::max(15, static_cast<int>(result.radius * 0.3));
            cv::line(debugImage_, Point(result.center.x - crossSize, result.center.y),
                     Point(result.center.x + crossSize, result.center.y), Scalar(0, 0, 255), 4);
            cv::line(debugImage_, Point(result.center.x, result.center.y - crossSize),
                     Point(result.center.x, result.center.y + crossSize), Scalar(0, 0, 255), 4);
            cv::circle(debugImage_, result.center, 5, Scalar(0, 0, 255), -1);
            cv::line(debugImage_, Point(imageCenter_.x - 15, imageCenter_.y),
                     Point(imageCenter_.x + 15, imageCenter_.y), Scalar(255, 100, 0), 3);
            cv::line(debugImage_, Point(imageCenter_.x, imageCenter_.y - 15),
                     Point(imageCenter_.x, imageCenter_.y + 15), Scalar(255, 100, 0), 3);
            // 第一行：追踪进度
            {
                std::string trackStr;
                if (tracking_)
                    trackStr = "Tracking  lost:" + std::to_string(lostFrames_) + "/" + std::to_string(requiredLostFrames_);
                else
                    trackStr = "Lock: " + std::to_string(consecutiveFrames_) + "/" + std::to_string(requiredFrames_);
                cv::putText(debugImage_, trackStr, Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.8, Scalar(0, 255, 0), 2);
            }
            // 第二行：偏转方向（仅追踪稳定后）
            if (tracking_)
            {
                std::string turnStr;
                if (result.turnFlag == 1.0f)       turnStr = "Turn: LEFT";
                else if (result.turnFlag == 2.0f)  turnStr = "Turn: RIGHT";
                else                               turnStr = "Turn: ALIGN";
                cv::putText(debugImage_, turnStr, Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.8, Scalar(0, 200, 255), 2);
            }
        }
    }
    else
    {
        consecutiveFrames_ = 0;
        if (tracking_)
        {
            lostFrames_++;
            if (lostFrames_ >= requiredLostFrames_)
                tracking_ = false;
        }
        if (drawDebug_) {
            StageTimer t(profileEnabled_, profileTimes_[7]);
            cv::line(debugImage_, Point(imageCenter_.x - 15, imageCenter_.y),
                     Point(imageCenter_.x + 15, imageCenter_.y), Scalar(255, 100, 0), 3);
            cv::line(debugImage_, Point(imageCenter_.x, imageCenter_.y - 15),
                     Point(imageCenter_.x, imageCenter_.y + 15), Scalar(255, 100, 0), 3);
            cv::circle(debugImage_, imageCenter_, 5, Scalar(255, 100, 0), -1);
            {
                std::string trackStr;
                if (tracking_)
                    trackStr = "Tracking  lost:" + std::to_string(lostFrames_) + "/" + std::to_string(requiredLostFrames_);
                else
                    trackStr = "No Target  lock:0/" + std::to_string(requiredFrames_);
                cv::putText(debugImage_, trackStr, Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.8, Scalar(0, 0, 255), 2);
            }
        }
    }

    return result;
}

Mat GreenDetect::preprocessGreenChannel(const Mat &image)
{
    Mat binary(image.size(), CV_8UC1);
    {
        // G > B 且 G > R 时，HSV 的 V 恰好为 G。保留原有浮点除法和严格大于比较，
        // 合并颜色条件以减少中间图与全图遍历。支持非连续 Mat 的逐行访问。
        StageTimer t(profileEnabled_, profileTimes_[2]);
        // 只并行独立的图像行，保留逐像素判定与浮点计算；限制任务数量避免过度并发。
        cv::parallel_for_(cv::Range(0, image.rows), [&](const cv::Range &rows) {
            for (int y = rows.start; y < rows.end; ++y) {
                const auto *src = image.ptr<cv::Vec3b>(y);
                auto *dst = binary.ptr<uchar>(y);
                for (int x = 0; x < image.cols; ++x) {
                    const int b = src[x][0], g = src[x][1], r = src[x][2];
                    dst[x] = g > brightThresh_ && g > b && g > r &&
                             static_cast<float>(g) / static_cast<float>(b + g + r + 1) > greenWeightThresh_ ? 255 : 0;
                }
            }
        }, 4.0);
    }

    // 全零掩码经过开闭运算仍为全零，可直接保留以供后续状态与调试输出使用。
    if (cv::countNonZero(binary) != 0)
    {
    StageTimer t(profileEnabled_, profileTimes_[4]);
    int kSize = (kernelSize_ % 2 == 0) ? kernelSize_ + 1 : kernelSize_;
    Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(kSize, kSize));
    for (int i = 0; i < openIterations_; i++)
        cv::morphologyEx(binary, binary, cv::MORPH_OPEN, kernel);
    for (int i = 0; i < closeIterations_; i++)
        cv::morphologyEx(binary, binary, cv::MORPH_CLOSE, kernel);
    }

    if (drawDebug_) binaryImage_ = binary.clone();
    else binaryImage_.release();
    return binary;
}

std::vector<Vec3f> GreenDetect::detectCircles(const Mat &binaryImage)
{
    std::vector<Vec3f> circles;
    candidateCount_ = 0;
    if (binaryImage.empty() || binaryImage.channels() != 1)
        return circles;

    std::vector<std::vector<Point>> contours;
    { StageTimer t(profileEnabled_, profileTimes_[5]); cv::findContours(binaryImage, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE); }
    if (contours.empty())
        return circles;

    double maxArea = 0;
    int bestIdx = -1;
    int passedCount = 0;
    for (int i = 0; i < (int)contours.size(); i++)
    {
        double area = cv::contourArea(contours[i]);
        if (area < 10)  // 噪点直接跳过，不绘制
            continue;

        double perimeter = cv::arcLength(contours[i], true);
        double circularity = (perimeter > 1.0) ? (4.0 * CV_PI * area / (perimeter * perimeter)) : 0;
        cv::Rect br = cv::boundingRect(contours[i]);
        double aspectRatio = (br.height > 0) ? (double)br.width / br.height : 0;
        Point2f dummy; float radius;
        cv::minEnclosingCircle(contours[i], dummy, radius);

        // 判断每个条件是否通过，决定颜色（绿=通过，红=被卡）
        bool okArea  = (area >= minArea_ && area <= maxArea_);
        bool okCirc  = (circularity >= minCircularity_);
        bool okAR    = (aspectRatio >= minAspectRatio_ && aspectRatio <= maxAspectRatio_);
        bool okAll   = okArea && okCirc && okAR;
        Scalar boxColor = okAll ? Scalar(0,255,0) : Scalar(0,80,255);

        if (!debugImage_.empty())
        {
            StageTimer t(profileEnabled_, profileTimes_[7]);
            cv::rectangle(debugImage_, br, boxColor, 1);
            // 每行一个参数，显示在轮廓框右侧或下方
            int tx = br.x + br.width + 4;
            int ty = br.y + 12;
            auto fmt1 = [](const char* name, double v, double lo, double hi) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%s:%.0f [%.0f~%.0f]", name, v, lo, hi);
                return std::string(buf);
            };
            auto fmt2 = [](const char* name, double v, double thresh) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%s:%.2f>%.2f", name, v, thresh);
                return std::string(buf);
            };
            Scalar cA = okArea ? Scalar(0,255,0) : Scalar(0,80,255);
            Scalar cC = okCirc ? Scalar(0,255,0) : Scalar(0,80,255);
            Scalar cR = okAR   ? Scalar(0,255,0) : Scalar(0,80,255);
            cv::putText(debugImage_, fmt1("A", area,    minArea_,       maxArea_),       Point(tx,ty),    cv::FONT_HERSHEY_SIMPLEX,0.38,cA,1);
            cv::putText(debugImage_, fmt1("R", radius,  minRadius_,     maxRadius_),     Point(tx,ty+14), cv::FONT_HERSHEY_SIMPLEX,0.38,Scalar(200,200,200),1);
            cv::putText(debugImage_, fmt2("C", circularity, minCircularity_),            Point(tx,ty+28), cv::FONT_HERSHEY_SIMPLEX,0.38,cC,1);
            cv::putText(debugImage_, fmt1("W", aspectRatio, minAspectRatio_,maxAspectRatio_), Point(tx,ty+42), cv::FONT_HERSHEY_SIMPLEX,0.38,cR,1);
        }

        if (!okArea || perimeter < 1.0 || !okCirc || !okAR)
            continue;

        passedCount++;
        if (area > maxArea)
        {
            maxArea = area;
            bestIdx = i;
        }
    }
    candidateCount_ = passedCount;
    if (bestIdx < 0)
        return circles;

    // 用矩计算质心（比minEnclosingCircle圆心更准确）
    cv::Moments m = cv::moments(contours[bestIdx]);
    if (m.m00 < 1.0)
        return circles;
    Point2f center(m.m10 / m.m00, m.m01 / m.m00);

    // 半径仍用minEnclosingCircle获取
    Point2f dummy;
    float radius;
    cv::minEnclosingCircle(contours[bestIdx], dummy, radius);

    if (radius >= minRadius_ && radius <= maxRadius_ &&
        center.x >= 0 && center.x < binaryImage.cols &&
        center.y >= 0 && center.y < binaryImage.rows)
        circles.push_back(Vec3f(center.x, center.y, radius));

    return circles;
}

void GreenDetect::calculateTurnDirection(const Point2f &targetCenter, TargetInfo &result)
{
    float deltaX = targetCenter.x - imageCenter_.x;

    if (std::abs(deltaX) < turnThreshold_)
    {
        // 停止
        result.turnFlag = 0.0f;
    }
    else if (deltaX < 0)
    {
        // 左转
        result.turnFlag = 1.0f;
    }
    else
    {
        // 右转
        result.turnFlag = 2.0f;
    }
}

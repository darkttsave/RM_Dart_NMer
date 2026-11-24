/**
 * @file FrameSource.cpp
 * @brief 可选模拟视频源实现（M1）
 */

#include "FrameSource.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

namespace webdebug
{
namespace
{
using Clock = std::chrono::steady_clock;

// 与 config/debug.yaml 的检测参数匹配：bright_thresh=80、半径范围 10~30、max_area=5000
constexpr int kWidth = 1440;
constexpr int kHeight = 1080;
constexpr double kTargetFps = 30.0;
constexpr float kLightRadius = 18.0f;

/** 合成绿色引导灯画面（带网格背景，便于目视确认调试画面是否实时刷新） */
class SyntheticFrameSource : public FrameSource
{
public:
    explicit SyntheticFrameSource(bool with_target)
        : with_target_(with_target)
    {
        kind_ = "synthetic";
        label_ = with_target_ ? "模拟视频源：合成绿色引导灯" : "模拟视频源：合成画面（无目标）";
        next_frame_ = Clock::now();
    }

    bool next(cv::Mat &out) override
    {
        // 按目标帧率节流，模拟真实相机帧间隔
        next_frame_ += std::chrono::microseconds(static_cast<int64_t>(1000000.0 / kTargetFps));
        const auto now = Clock::now();
        if (next_frame_ > now)
            std::this_thread::sleep_for(next_frame_ - now);
        else
            next_frame_ = now;

        const double t = std::chrono::duration<double>(now.time_since_epoch()).count();

        out = cv::Mat(kHeight, kWidth, CV_8UC3, cv::Scalar(10, 12, 14));

        // 网格背景
        const cv::Scalar gridColor(34, 38, 42);
        for (int x = 0; x < kWidth; x += 120)
            cv::line(out, cv::Point(x, 0), cv::Point(x, kHeight), gridColor, 1);
        for (int y = 0; y < kHeight; y += 120)
            cv::line(out, cv::Point(0, y), cv::Point(kWidth, y), gridColor, 1);

        // 画面中心十字
        const cv::Point2f center(kWidth / 2.0f, kHeight / 2.0f);
        cv::line(out, cv::Point(center.x - 30, center.y), cv::Point(center.x + 30, center.y),
                 cv::Scalar(90, 90, 90), 1);
        cv::line(out, cv::Point(center.x, center.y - 30), cv::Point(center.x, center.y + 30),
                 cv::Scalar(90, 90, 90), 1);

        if (with_target_)
        {
            const float x = center.x + 320.0f * static_cast<float>(std::sin(t * 2.0 * CV_PI / 5.0));
            const float y = center.y + 120.0f * static_cast<float>(std::sin(t * 2.0 * CV_PI / 3.2));
            // 光晕 + 灯芯（BGR 纯绿，满足 V>80 与 G>R/G>B 条件）
            cv::circle(out, cv::Point(static_cast<int>(x), static_cast<int>(y)),
                       static_cast<int>(kLightRadius * 1.8f), cv::Scalar(0, 60, 0), -1);
            cv::circle(out, cv::Point(static_cast<int>(x), static_cast<int>(y)),
                       static_cast<int>(kLightRadius), cv::Scalar(0, 255, 0), -1);
        }

        // 模拟相机 OSD（只画在合成画面上，真实相机画面由相机自己输出）
        char osd[128];
        std::snprintf(osd, sizeof(osd), "SYNTHETIC CAMERA  %dx%d  %.1f FPS  %s",
                      kWidth, kHeight, kTargetFps, with_target_ ? "TARGET: ON" : "TARGET: OFF");
        cv::putText(out, osd, cv::Point(16, kHeight - 20), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                    cv::Scalar(120, 200, 120), 1);
        return true;
    }

    const std::string &sourceKind() const override { return kind_; }
    const std::string &sourceLabel() const override { return label_; }

private:
    bool with_target_;
    std::string kind_;
    std::string label_;
    Clock::time_point next_frame_;
};

class ImageFrameSource : public FrameSource
{
public:
    ImageFrameSource(const std::string &path, cv::Mat image)
        : image_(std::move(image))
    {
        kind_ = "sim_image";
        label_ = "模拟视频源：测试图片 " + path;
    }

    bool next(cv::Mat &out) override
    {
        if (image_.empty())
            return false;
        // 每帧返回独立副本，避免下游标注修改源图
        out = image_.clone();
        return true;
    }

    const std::string &sourceKind() const override { return kind_; }
    const std::string &sourceLabel() const override { return label_; }

private:
    cv::Mat image_;
    std::string kind_;
    std::string label_;
};

class VideoFrameSource : public FrameSource
{
public:
    explicit VideoFrameSource(const std::string &path)
        : cap_(path)
    {
        kind_ = "sim_video";
        label_ = "模拟视频源：视频文件 " + path;
    }

    bool isOpened() const { return cap_.isOpened(); }

    bool next(cv::Mat &out) override
    {
        looped_ = false;
        if (!cap_.isOpened())
            return false;
        if (!cap_.read(out) || out.empty())
        {
            // 循环播放
            cap_.set(cv::CAP_PROP_POS_FRAMES, 0);
            looped_ = true;
            return cap_.read(out) && !out.empty();
        }
        return true;
    }

    const std::string &sourceKind() const override { return kind_; }
    const std::string &sourceLabel() const override { return label_; }
    bool looped() const override { return looped_; }
    std::optional<double> sourceTimeMs() const override
    {
        const double fps = cap_.get(cv::CAP_PROP_FPS);
        const double position = cap_.get(cv::CAP_PROP_POS_FRAMES);
        if (fps > 0.0 && position >= 1.0)
            return (position - 1.0) * 1000.0 / fps;
        return std::nullopt;
    }

private:
    cv::VideoCapture cap_;
    bool looped_ = false;
    std::string kind_;
    std::string label_;
};

} // namespace

std::unique_ptr<FrameSource> createFrameSource(const std::string &spec, std::string *error)
{
    auto fail = [&](const std::string &reason) -> std::unique_ptr<FrameSource> {
        if (error != nullptr)
            *error = reason;
        return nullptr;
    };

    if (spec.empty() || spec == "synthetic" || spec == "sim")
        return std::make_unique<SyntheticFrameSource>(true);
    if (spec == "synthetic:none" || spec == "sim:none")
        return std::make_unique<SyntheticFrameSource>(false);

    const std::string imagePrefix = "image:";
    if (spec.rfind(imagePrefix, 0) == 0)
    {
        const std::string path = spec.substr(imagePrefix.size());
        cv::Mat image = cv::imread(path, cv::IMREAD_COLOR);
        if (image.empty())
            return fail("无法读取测试图片: " + path);
        if (image.cols < 100 || image.rows < 100)
            return fail("测试图片尺寸过小（需 >=100px）: " + path);
        return std::make_unique<ImageFrameSource>(path, std::move(image));
    }

    const std::string videoPrefix = "video:";
    if (spec.rfind(videoPrefix, 0) == 0)
    {
        const std::string path = spec.substr(videoPrefix.size());
        auto source = std::make_unique<VideoFrameSource>(path);
        if (!source->isOpened())
            return fail("无法打开视频文件: " + path + "（请检查路径、文件及视频编码）");
        return source;
    }

    return fail("未知模拟视频源: " + spec + "（支持 synthetic / synthetic:none / image:<path> / video:<path>）");
}

} // namespace webdebug

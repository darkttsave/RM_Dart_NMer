#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

// Completion counts and measured elapsed time, not averages of instant FPS.
class WebPerformanceWindow
{
public:
    using Clock = std::chrono::steady_clock;
    explicit WebPerformanceWindow(Clock::time_point start) : processingStart_(start), receiveStart_(start) {}
    void received(Clock::time_point now)
    {
        ++receiveCount_;
        const double seconds = std::chrono::duration<double>(now - receiveStart_).count();
        if (seconds >= 1.0) {
            hostFps = receiveCount_ / seconds;
            receiveCount_ = 0;
            receiveStart_ = now;
        }
    }
    void processed(Clock::time_point now, double processingMs)
    {
        ++processingCount_;
        durations_.push_back(processingMs);
        const double seconds = std::chrono::duration<double>(now - processingStart_).count();
        if (seconds >= 5.0) {
            framesInWindow = processingCount_;
            windowSeconds = seconds;
            processingFps = processingCount_ / seconds;
            const auto index = static_cast<std::size_t>(std::ceil(durations_.size() * 0.95)) - 1;
            std::nth_element(durations_.begin(), durations_.begin() + index, durations_.end());
            processingP95Ms = durations_[index];
            durations_.clear();
            processingCount_ = 0;
            processingStart_ = now;
        }
    }
    std::optional<double> processingFps, processingP95Ms, hostFps, windowSeconds;
    std::optional<std::uint64_t> framesInWindow;
private:
    Clock::time_point processingStart_, receiveStart_;
    std::uint64_t processingCount_ = 0, receiveCount_ = 0;
    std::vector<double> durations_;
};

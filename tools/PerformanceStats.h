#pragma once

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <vector>

// Opt-in, bounded periodic samples; FPS is the reciprocal of mean duration,
// not an assertion about the camera sensor's physical acquisition rate.
class PerformanceStats
{
public:
    using Clock = std::chrono::steady_clock;
    explicit PerformanceStats(bool enabled) : enabled_(enabled), start_(Clock::now()) {}
    bool enabled() const { return enabled_; }
    static double ms(Clock::time_point a, Clock::time_point b)
    { return std::chrono::duration<double, std::milli>(b - a).count(); }
    void add(std::vector<double> &samples, double value)
    { if (enabled_) samples.push_back(value); }
    bool flush(bool force = false)
    {
        if (!enabled_ || (!force && Clock::now() - start_ < std::chrono::seconds(5))) return false;
        report("getimage", capture); report("sdk_wait", sdk_wait); report("conversion", conversion);
        report("green", green); report("dart", dart);
        report("debug_overlay", overlay); report("web_and_record_submit", submit);
        report("main_loop", loop);
        start_ = Clock::now();
        return true;
    }
    std::vector<double> capture, sdk_wait, conversion, green, dart, overlay, submit, loop;
private:
    void report(const char *name, std::vector<double> &v)
    {
        if (v.empty()) return;
        const double mean = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
        std::sort(v.begin(), v.end());
        std::cout << "[Perf] " << name << " n=" << v.size() << " mean_ms=" << mean
                  << " p50_ms=" << v[(v.size()-1)/2] << " p95_ms=" << v[(v.size()-1)*95/100]
                  << " max_ms=" << v.back() << " reciprocal_fps=" << (mean > 0 ? 1000.0/mean : 0) << std::endl;
        v.clear();
    }
    bool enabled_;
    Clock::time_point start_;
};

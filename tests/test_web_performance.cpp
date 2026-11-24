#include "tools/WebPerformanceWindow.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
int main() {
    using Clock = WebPerformanceWindow::Clock;
    auto t = Clock::time_point{};
    WebPerformanceWindow stats(t);
    assert(!stats.processingFps && !stats.hostFps && !stats.processingP95Ms);
    stats.received(t + std::chrono::milliseconds(400));
    stats.received(t + std::chrono::milliseconds(1100));
    assert(stats.hostFps && std::abs(*stats.hostFps - 2.0 / 1.1) < 1e-9);
    for (int i=1; i<=19; ++i) stats.processed(t + std::chrono::milliseconds(250*i), i);
    assert(!stats.processingFps);
    stats.processed(t + std::chrono::milliseconds(5500), 20);
    assert(stats.processingFps && std::abs(*stats.processingFps - 20.0 / 5.5) < 1e-9);
    assert(stats.framesInWindow && *stats.framesInWindow == 20);
    assert(stats.processingP95Ms && *stats.processingP95Ms == 19);
    stats.processed(t + std::chrono::milliseconds(6000), 100);
    assert(*stats.processingP95Ms == 19); // last completed window until next update
    std::cout << "PASS 实际帧数/时间、主机接收窗口和 P95\n";
}

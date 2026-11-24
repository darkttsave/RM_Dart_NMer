#include "tasks/dart/DartDetector.h"
#include "tasks/dart/DartTracker.h"
#include "debug/FrameSource.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <iostream>

using namespace dartvision;
static cv::Mat scene(cv::Scalar color, int x, bool moving = true) {
    cv::Mat image = cv::Mat::zeros(240, 320, CV_8UC3);
    cv::rectangle(image, {250, 30, 16, 12}, color, -1); // 静态干扰
    if (moving) cv::rectangle(image, {x, 120, 18, 10}, color, -1);
    return image;
}
int main() {
    for (const auto &pair : {std::make_pair(std::string("red"), cv::Scalar(0,0,255)),
                             std::make_pair(std::string("blue"), cv::Scalar(255,0,0))}) {
        DetectorConfig dc; dc.color = pair.first;
        DartDetector detector(dc); TrackerConfig tc; tc.max_distance=50; tc.max_lost_frames=2;
        cv::Mat blurred = scene(pair.second, 30);
        cv::GaussianBlur(blurred, blurred, {9,9}, 2.0);
        assert(!detector.detect(blurred).empty());
        DartTracker tracker(tc);
        auto a=detector.detect(scene(pair.second,30)); assert(a.size()==2);
        auto r=tracker.update(a, 1, 1000, 0.0); assert(!r.detected && !r.track_valid && r.track_state=="waiting");
        r=tracker.update(detector.detect(scene(pair.second,40)),2,2000,33.3);
        assert(r.detected && r.track_valid && r.track_state=="captured" && r.track_points.size()==2 && r.center->x < 100);
        r=tracker.update(detector.detect(scene(pair.second,50)),3,3000,66.6);
        assert(r.detected && r.track_state=="tracking" && r.track_points.size()==3);
        r=tracker.update(detector.detect(scene(pair.second,0,false)),4,4000,99.9);
        assert(!r.detected && !r.center && r.track_valid && r.track_state=="lost" && r.failure_reason=="association_failed");
        r=tracker.update(detector.detect(scene(pair.second,70)),5,5000,133.2);
        assert(r.detected && r.track_state=="recaptured" && r.track_points.back().frame_id==5);
        for (int i=6;i<=8;++i) r=tracker.update({},i,i*1000);
        assert(!r.detected && !r.center && !r.track_valid && r.track_state=="failed" && r.failure_reason=="track_timeout");
        tracker.update(detector.detect(scene(pair.second,100)),9,9000);
        r=tracker.update(detector.detect(scene(pair.second,110)),10,10000);
        assert(r.track_state=="captured" && r.track_points.size()==2 && r.track_points.front().frame_id==9);
        tracker.reset(); r=tracker.update({},11,11000);
        assert(!r.track_valid && r.track_points.empty() && r.track_state=="waiting");
        DartTracker stationary(tc);
        for (int i=1;i<=15;++i) {
            r=stationary.update(detector.detect(scene(pair.second,0,false)),i,i*1000);
            assert(!r.track_valid && !r.detected && !r.center);
        }
        DartTracker direction(tc);
        direction.update(detector.detect(scene(pair.second, 30)), 1, 1000);
        direction.update(detector.detect(scene(pair.second, 40)), 2, 2000);
        r=direction.update(detector.detect(scene(pair.second, 25)), 3, 3000);
        assert(!r.detected && r.track_state=="lost" && r.failure_reason=="association_failed");
        std::cout << "PASS " << pair.first << " 检测、静止干扰、多候选、丢失、重捕获、超时及新轨迹\n";
    }
    DetectorConfig red; DartDetector detector(red);
    assert(detector.detect(scene({255,0,0},30)).empty());
    DartTracker empty; for (int i=1;i<=5;++i) assert(!empty.update({},i,i*1000).track_valid);
    const auto path=std::filesystem::temp_directory_path()/"dart-p2-loop-test.avi";
    { cv::VideoWriter v(path.string(),cv::VideoWriter::fourcc('M','J','P','G'),30,{320,240});
      assert(v.isOpened()); v.write(scene({0,0,255},30));v.write(scene({0,0,255},40)); }
    auto source=webdebug::createFrameSource("video:"+path.string()); cv::Mat frame;
    assert(source && source->next(frame) && !source->looped() && source->sourceTimeMs());
    assert(source->next(frame) && !source->looped());
    assert(source->next(frame) && source->looped() && source->sourceTimeMs() && *source->sourceTimeMs()<1.0);
    source.reset();std::filesystem::remove(path);
    std::cout << "PASS 空画面、颜色隔离与视频回绕事件\n";
}

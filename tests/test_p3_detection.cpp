#include "tasks/dart/DartDetector.h"
#include "tasks/dart/DartTracker.h"
#include "tasks/dart/GreenDetect.h"
#include <opencv2/imgproc.hpp>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using namespace dartvision;
static Candidate point(float x, float y) { Candidate c; c.center={x,y}; c.bbox=cv::Rect(int(x),int(y),1,1); return c; }
int main() {
    cv::Mat image=cv::Mat::zeros(240,320,CV_8UC3);
    cv::rectangle(image, cv::Rect(30,80,40,10), {0,0,255}, -1);
    cv::circle(image, {230,90}, 15, {0,0,255}, -1);
    DetectorConfig cfg; cfg.close_kernel=1; DartDetector baseline(cfg);
    auto all=baseline.detect(image); assert(all.size()==2);
    cfg.roi_enabled=true; cfg.roi=cv::Rect(20,60,70,50);
    auto inside=DartDetector(cfg).detect(image); assert(inside.size()==1);
    assert(inside[0].bbox.x==30 && inside[0].bbox.y==80 && inside[0].center.x==50);
    cfg.roi=cv::Rect(100,100,10,10); assert(DartDetector(cfg).detect(image).empty());
    cfg.roi=cv::Rect(310,220,100,100); DetectionDiagnostics diag;
    assert(DartDetector(cfg).detect(image,&diag).empty() && diag.invalid_roi);
    cfg.roi_enabled=false; cfg.circularity_filter_enabled=true; cfg.max_circularity=0.8;
    auto filtered=DartDetector(cfg).detect(image,&diag);
    assert(filtered.size()==1 && filtered[0].bbox.x==30 && diag.circularity_rejected==1);
    cfg.circularity_filter_enabled=false; assert(DartDetector(cfg).detect(image).size()==2);

    GreenDetect green; green.setDebugRendering(false); green.setRequiredFrames(1);
    green.setMorphology(1,0,0); green.setRadiusFilter(5,30); green.setAreaFilter(10,5000);
    cv::Mat g=cv::Mat::zeros(240,320,CV_8UC3); cv::circle(g,{210,90},12,{0,255,0},-1);
    green.setRoi(true,cv::Rect(180,60,60,60)); auto a=green.detectGreenTarget(g);
    assert(a.detected && std::abs(a.center.x-210)<1 && std::abs(green.getImageCenter().x-160)<0.1);
    green.setRoi(true,cv::Rect(0,0,60,60)); assert(!green.detectGreenTarget(g).detected);
    green.setRoi(true,cv::Rect(300,200,40,100)); assert(!green.detectGreenTarget(g).detected && green.roiInvalid());
    green.setRoi(false,{}); assert(green.detectGreenTarget(g).detected);

    TrackerConfig tc; tc.direction_enabled=true;tc.direction_observation_points=3;tc.direction_min_displacement=5;tc.min_motion=1;tc.max_distance=100;
    DartTracker up(tc); up.update({point(50,100)},1,1); auto t=up.update({point(51,90)},2,2);assert(t.track_valid);
    const auto count=t.track_points.size();t=up.update({point(52,80)},3,3);
    assert(!t.detected && t.failure_reason=="direction_rejected" && t.track_points.size()==count);
    DartTracker down(tc);down.update({point(50,50)},1,1);down.update({point(51,60)},2,2);
    t=down.update({point(52,58)},3,3);assert(t.detected); // 短暂反向抖动
    t=down.update({},4,4);assert(t.track_state=="lost");
    t=down.update({point(54,70)},5,5);assert(t.detected && t.track_state=="recaptured");
    std::cout<<"PASS P3 ROI、圆形度、方向约束及坐标安全处理\n";
}

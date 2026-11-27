#include "tasks/dart/Observation.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
using namespace dartvision;
int main() {
    FlightWindow continuous; assert(continuous.allowsNew(0) && continuous.allowsExisting(0));
    FlightWindow window({true,100,200,500}); assert(!window.allowsNew(0));
    assert(window.trigger(1,1000)); assert(!window.trigger(1,1100));
    assert(!window.allowsNew(1099) && window.allowsNew(1100) && !window.allowsNew(1300));
    window.established(1250);assert(window.allowsExisting(1301) && !window.allowsExisting(1751));
    assert(window.trigger(2,2000) && !window.allowsExisting(2000));assert(!window.trigger(1,2100));window.reset();assert(!window.allowsNew(2200));
    TrackPoint a{1,1000,10.0,{20,10}},b{2,2000,20.0,{20,30}};
    ReferenceLine line{"A",{0,20},{40,20}};auto r=measureCrossing(line,a,b);
    assert(r.valid && r.point.x==20 && r.point.y==20 && r.after_frame==2 && *r.interpolated_source_ms==15);
    assert(!measureCrossing({"A",{0,20},{10,20}},a,b).valid);
    assert(measureCrossing({"A",{0,20},{0,20}},a,b).reason=="invalid_line");
    b.frame_id=4;assert(measureCrossing(line,a,b).reason=="frame_gap_or_time_invalid");
    b.frame_id=2;b.center={20,5};assert(!measureCrossing(line,a,b).valid);
    a={2,2000,20.0,{20,20}};b={3,3000,30.0,{20,30}};assert(!measureCrossing(line,a,b).valid);
    a={3,3000,30.0,{20,30}};b={4,4000,40.0,{20,10}};assert(measureCrossing(line,a,b).valid);
    std::cout<<"PASS 模拟事件窗口、有限参考线交点及时间/帧间隔校验\n";
}

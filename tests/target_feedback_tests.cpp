#include "arc/target_feedback.hpp"
#include <limits>
int main(){
    arc::TargetFeedbackGate g;
    g.observe(18,16);if(g.reduce())return 1;
    g.observe(16,16);g.observe(18,16);if(g.reduce())return 2;
    g.observe(18,16);if(!g.reduce())return 3;
    g.reset();for(int i=0;i<2;++i)g.observe(10,16);if(g.recover())return 4;
    g.observe(10,16);if(!g.recover())return 5;
    g.observe(15,16);if(g.recover())return 6;
    if(arc::reject_feedback_change(10,15,16,true))return 7;
    if(!arc::reject_feedback_change(10,18,16,true))return 8;
    if(!arc::reject_feedback_change(10,11,16,false))return 9;
    if(arc::reject_feedback_change(10,9,16,false))return 10;
    if(!arc::reject_feedback_change(10,std::numeric_limits<double>::quiet_NaN(),16,false))return 11;
    g.observe(18,16);g.observe(-1,16);if(g.reduce()||g.recover())return 12;
}

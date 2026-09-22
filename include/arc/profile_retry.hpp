#pragma once
#include <algorithm>
namespace arc {
class ProfileRetry {
    unsigned failures_{};
public:
    void complete(bool usable){failures_=usable?0:std::min(5u,failures_+1);}
    unsigned failures()const{return failures_;}
    unsigned delay_seconds()const{return std::min(30u,2u<<failures_);}
    bool unavailable(double seconds,bool active_policy)const{return !active_policy&&failures_>=3&&seconds>=30.;}
};
}

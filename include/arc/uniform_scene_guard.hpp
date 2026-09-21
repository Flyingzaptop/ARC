#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
namespace arc {
struct UniformChange {
    unsigned compared{},large{},severe{};
    bool changed()const{return compared>=3&&(large>=2||severe>=1);}
};
// Conservative scene-change proxy, not shader semantics or a visibility proof.
// Only actually used, readable float components participate; padding and small
// integer bit patterns cannot manufacture changes.
template<class Words>void accumulate_uniform_change(const Words& before,const Words& after,UniformChange& result){
    for(unsigned i=0;i<4;++i)if(before.valid_mask&after.valid_mask&(1u<<i)){
        const float a=std::bit_cast<float>(before.words[i]),b=std::bit_cast<float>(after.words[i]);
        if(!std::isfinite(a)||!std::isfinite(b)||std::max(std::abs(a),std::abs(b))<.0001f||std::max(std::abs(a),std::abs(b))>1.e8f)continue;
        ++result.compared;const auto difference=std::abs(a-b)/std::max({1.f,std::abs(a),std::abs(b)});
        result.large+=difference>=.25f;result.severe+=difference>=.75f;
    }
}
}

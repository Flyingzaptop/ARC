#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
namespace arc {
struct SpatialModelEntry {float error{1000};std::uint32_t samples{},last_frame{},invalid{};};
struct SpatialModelTable {std::uint64_t key{};std::uint32_t version{1},reserved{};std::array<SpatialModelEntry,256> entries;};
static_assert(sizeof(SpatialModelEntry)==16&&sizeof(SpatialModelTable)==4112);
class SpatialSensitivity {
    struct Bin {double error{},noise{};std::uint64_t last_frame{},last_noise_frame{};std::uint32_t samples{},noise_samples{};bool invalid{};};
    std::uint64_t key_{},revision_{};std::array<Bin,256> bins_{};
public:
    explicit SpatialSensitivity(std::uint64_t key=0):key_(key){}
    void reset(std::uint64_t key){key_=key;bins_={};++revision_;}
    bool observe(std::uint64_t key,std::uint64_t frame,unsigned bin,double error,bool neutral_noise,bool valid=true){
        if(!key||key!=key_||!frame||bin>=bins_.size()||!std::isfinite(error)||error<0)return false;
        auto& b=bins_[bin];if(frame<(neutral_noise?b.last_noise_frame:b.last_frame))return false;if(!valid){b.invalid=true;++revision_;return true;}
        if(neutral_noise){b.noise=std::max(b.noise,error);if(frame!=b.last_noise_frame){b.last_noise_frame=frame;if(b.noise_samples<UINT32_MAX)++b.noise_samples;}}
        else{b.error=std::max(b.error,error);if(frame!=b.last_frame){b.last_frame=frame;if(b.samples<UINT32_MAX)++b.samples;}}
        ++revision_;return true;
    }
    [[nodiscard]] std::uint64_t revision()const{return revision_;}
    [[nodiscard]] SpatialModelTable snapshot()const{
        SpatialModelTable table;table.key=key_;table.reserved=std::uint32_t(revision_);
        for(unsigned i=0;i<bins_.size();++i){const auto& b=bins_[i];auto& entry=table.entries[i];entry.samples=b.noise_samples?b.samples:0;entry.invalid=b.invalid;entry.last_frame=std::uint32_t(b.last_frame);
            if(!b.invalid&&b.noise_samples&&b.samples>=3)entry.error=std::nextafter(float(b.error+2*b.noise),std::numeric_limits<float>::infinity());}
        return table;
    }
};
}

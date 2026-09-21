#pragma once
#include "arc/descriptor_ledger.hpp"
#include <set>
#include <map>
#include <algorithm>

namespace arc {
// A retained policy cannot learn new texture generations or views.
class PolicyBindingEvidence {
    struct Signature {std::uint64_t pipeline{};std::vector<DescriptorValue> views;auto operator<=>(const Signature&)const=default;};
    struct Entry {bool sealed{},invalid{};std::set<Signature> bindings;};
    std::map<std::uint64_t,Entry> entries_;
    std::uint64_t revision_{};
public:
    bool observe(std::uint64_t policy,std::uint64_t pipeline,std::vector<DescriptorValue> views){
        if(!policy||!pipeline||views.empty()||views.size()>128)return false;
        auto found=entries_.find(policy);
        if(found==entries_.end()){
            if(entries_.size()>=64){auto victim=std::find_if(entries_.begin(),entries_.end(),[](const auto& e){return !e.second.sealed;});if(victim==entries_.end())return false;entries_.erase(victim);}
            found=entries_.emplace(policy,Entry{}).first;
        }
        auto& entry=found->second;if(entry.invalid)return false;
        Signature value{pipeline,std::move(views)};
        if(entry.bindings.contains(value))return true;
        if(entry.sealed||entry.bindings.size()>=64){entry.invalid=true;++revision_;return false;}
        entry.bindings.insert(std::move(value));return true;
    }
    bool seal(std::uint64_t policy){const auto found=entries_.find(policy);if(found==entries_.end()||found->second.invalid||found->second.bindings.empty())return false;found->second.sealed=true;return true;}
    void retire(std::uint64_t resource){for(auto& [id,entry]:entries_){(void)id;if(entry.invalid)continue;for(const auto& b:entry.bindings)if(std::any_of(b.views.begin(),b.views.end(),[&](const auto& v){return v.resource==resource||v.shape.counter_resource==resource;})){entry.invalid=true;++revision_;break;}}}
    void retire_pipeline(std::uint64_t pipeline){for(auto& [id,entry]:entries_){(void)id;if(!entry.invalid&&std::any_of(entry.bindings.begin(),entry.bindings.end(),[&](const auto& b){return b.pipeline==pipeline;})){entry.invalid=true;++revision_;}}}
    void clear(){entries_.clear();}
    std::uint64_t revision()const noexcept{return revision_;}
};
}

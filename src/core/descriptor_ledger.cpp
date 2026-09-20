#include "arc/descriptor_ledger.hpp"
#include <limits>
#include <array>

namespace arc {
const DescriptorLedger::Heap* DescriptorLedger::locate_heap(std::uint64_t address)const {
    // Cache only numeric hints, never heap pointers. A fresh map lookup and
    // bounds check make copied ledgers, retirement and address reuse safe.
    struct Hint {std::uint64_t id{},start{},end{};};
    struct Cache {const DescriptorLedger* owner{};std::array<Hint,4> hints{};unsigned next{};};
    thread_local Cache cache;if(cache.owner!=this){cache={};cache.owner=this;}
    for(auto& hint:cache.hints)if(hint.id&&address>=hint.start&&address<hint.end){
        const auto found=heaps_.find(hint.id);
        if(found!=heaps_.end()){const auto& h=found->second;if(address>=h.start){const auto offset=address-h.start;if(offset%h.stride==0&&offset/h.stride<h.entries.size())return &h;}}
        hint={};
    }
    auto it=starts_.upper_bound(address);if(it==starts_.begin())return nullptr;--it;
    const auto& h=heaps_.at(it->second);const auto offset=address-h.start;
    if(offset%h.stride||offset/h.stride>=h.entries.size())return nullptr;
    cache.hints[cache.next++%cache.hints.size()]={h.id,h.start,h.start+std::uint64_t(h.entries.size())*h.stride};return &h;
}
std::uint64_t DescriptorLedger::heap_at(std::uint64_t address)const {const auto* heap=locate_heap(address);return heap?heap->id:0;}
bool DescriptorLedger::register_heap(std::uint64_t id,std::uint64_t start,std::uint32_t stride,std::uint32_t count){
    if(!id||!start||!stride||!count||heaps_.size()>=limits_.heaps||heaps_.contains(id)||
       std::uint64_t(count)*stride>UINT64_MAX-start||count>limits_.slots-slots_)return false;
    const auto end=start+std::uint64_t(count)*stride;
    auto it=starts_.lower_bound(start);if(it!=starts_.end()&&it->first<end)return false;
    if(it!=starts_.begin()){auto before=std::prev(it);const auto& h=heaps_.at(before->second);if(h.start+std::uint64_t(h.entries.size())*h.stride>start)return false;}
    Heap heap;heap.id=id;heap.start=start;heap.stride=stride;heap.entries.resize(count);
    heaps_.emplace(id,std::move(heap));starts_[start]=id;slots_+=count;
    // Prior untracked addresses do not prove this new heap's contents. Discard,
    // never silently promote potentially reused native storage to known state.
    auto old=orphans_.lower_bound(start);while(old!=orphans_.end()&&old->first<end){release(old->second);--known_;old=orphans_.erase(old);}
    return true;
}
std::uint32_t DescriptorLedger::intern(DescriptorValue value){
    if(const auto it=lookup_.find(value);it!=lookup_.end()){++values_[it->second].references;if(!value.kind)++nulls_;return it->second;}
    if(lookup_.size()>=limits_.values||values_.size()>UINT32_MAX)return 0;
    std::uint32_t id;
    if(free_.empty()){id=static_cast<std::uint32_t>(values_.size());values_.push_back({value,1});}
    else{id=free_.back();free_.pop_back();values_[id]={value,1};}
    lookup_[value]=id;if(!value.kind)++nulls_;return id;
}
void DescriptorLedger::release(std::uint32_t id,std::size_t count){
    if(!id)return;auto& v=values_[id];if(!v.data.kind)nulls_-=count;v.references-=count;
    if(!v.references){lookup_.erase(v.data);free_.push_back(id);}
}
bool DescriptorLedger::write(std::uint64_t address,DescriptorValue value){
    if(!address)return false;auto* heap=locate_heap(address);
    if(!heap){auto it=starts_.upper_bound(address);if(it!=starts_.begin()){--it;const auto& h=heaps_.at(it->second);
        if(address-h.start<std::uint64_t(h.entries.size())*h.stride)return false;}}
    if(!heap&&!orphans_.contains(address)&&orphans_.size()>=limits_.orphan_slots)return false;
    std::uint32_t old=0;
    if(heap){const auto& h=*heap;old=h.entries[(address-h.start)/h.stride];}
    else if(auto it=orphans_.find(address);it!=orphans_.end())old=it->second;
    if(old&&values_[old].data==value)return true;
    const auto id=intern(value);if(!id)return false;
    if(heap){auto& h=*heap;auto& entry=h.entries[(address-h.start)/h.stride];entry=id;
        if(old){auto found=h.references.find(old);if(--found->second==0)h.references.erase(found);}++h.references[id];}
    else orphans_[address]=id;
    if(!old)++known_;release(old);return true;
}
void DescriptorLedger::forget(std::uint64_t address){
    auto* heap=locate_heap(address);
    if(heap){auto& h=*heap;auto& entry=h.entries[(address-h.start)/h.stride];if(!entry)return;
        auto found=h.references.find(entry);if(--found->second==0)h.references.erase(found);release(entry);entry=0;--known_;}
    else if(auto it=orphans_.find(address);it!=orphans_.end()){release(it->second);orphans_.erase(it);--known_;}
}
bool DescriptorLedger::copy_one(std::uint64_t destination,std::uint64_t source){
    if(!destination)return false;
    auto* source_heap=locate_heap(source);auto* destination_heap=locate_heap(destination);std::uint32_t value=0,old=0;
    if(source_heap){const auto& h=*source_heap;value=h.entries[(source-h.start)/h.stride];}
    else if(const auto it=orphans_.find(source);it!=orphans_.end())value=it->second;
    if(!value){forget(destination);return true;}
    Heap* target=nullptr;std::size_t index=0;
    if(destination_heap){target=destination_heap;index=(destination-target->start)/target->stride;old=target->entries[index];}
    else{
        auto it=starts_.upper_bound(destination);if(it!=starts_.begin()){--it;const auto& h=heaps_.at(it->second);if(destination-h.start<std::uint64_t(h.entries.size())*h.stride)return false;}
        const auto orphan=orphans_.find(destination);if(orphan!=orphans_.end())old=orphan->second;else if(orphans_.size()>=limits_.orphan_slots)return false;
    }
    if(old==value)return true;
    // Retain before releasing the old destination, including self/alias copies.
    ++values_[value].references;if(!values_[value].data.kind)++nulls_;
    if(target){++target->references[value];target->entries[index]=value;if(old){auto it=target->references.find(old);if(--it->second==0)target->references.erase(it);}}
    else orphans_[destination]=value;
    if(!old)++known_;release(old);return true;
}
std::optional<DescriptorValue> DescriptorLedger::read(std::uint64_t address)const{
    const auto* heap=locate_heap(address);std::uint32_t id=0;
    if(heap){const auto& h=*heap;id=h.entries[(address-h.start)/h.stride];}
    else if(auto it=orphans_.find(address);it!=orphans_.end())id=it->second;
    return id?std::optional<DescriptorValue>{values_[id].data}:std::nullopt;
}
void DescriptorLedger::retire_heap(std::uint64_t id){
    auto it=heaps_.find(id);if(it==heaps_.end())return;
    for(const auto& [value,count]:it->second.references){release(value,count);known_-=count;}
    slots_-=it->second.entries.size();starts_.erase(it->second.start);heaps_.erase(it);
}
}

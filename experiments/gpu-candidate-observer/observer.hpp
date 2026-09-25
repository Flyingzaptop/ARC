#pragma once
// Engine-independent observer core. Inputs are native D3D12 events only.
#include <windows.h>
#include <wrl/client.h>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include "../cpu-gpu-correspondence/memory_windows.hpp"
namespace arc_gpu_candidates {
using Microsoft::WRL::ComPtr;using Clock=std::chrono::steady_clock;
inline const std::string& path(){static const std::string& v=*new std::string([](){char b[2048]{};GetEnvironmentVariableA("ARC_GPU_CANDIDATES",b,2048);return std::string(b);}());return v;}
inline void check(HRESULT h){if(FAILED(h))throw std::runtime_error("observer D3D12 failure");}
struct Resource{uint64_t id{},generation{},bytes{},copies{},copied{},readable{},bound{};void* mapped{};bool upload{},selected{},pending{};unsigned captures{};};
struct Queue{uint64_t id{},value{};ComPtr<ID3D12Fence> fence;};
struct Snapshot{uint64_t id{},resource{},generation{},write{},range{},bytes{},list{},recording{},queue{},fence{};bool submitted{},complete{},invalid{};ComPtr<ID3D12Resource> readback,source;};
inline std::mutex& mutex=*new std::mutex;inline auto& resources=*new std::unordered_map<ID3D12Resource*,Resource>;
inline auto& recordings=*new std::unordered_map<ID3D12CommandList*,uint64_t>;
inline auto& queues=*new std::unordered_map<ID3D12CommandQueue*,Queue>;
inline auto& snapshots=*new std::vector<Snapshot>;inline uint64_t nextId{},operation{},lines{};inline unsigned selected{};inline Clock::time_point start=Clock::now();
inline void log(const std::string& fields,bool critical=false){if(!critical&&lines>=20000)return;static std::ofstream& f=*new std::ofstream(path()+"/events.jsonl");f<<"{\"event_id\":"<<++operation<<",\"time_ms\":"<<std::chrono::duration<double,std::milli>(Clock::now()-start).count()<<','<<fields<<"}\n";f.flush();++lines;}
inline void created(ID3D12Resource* p,void* mapped,bool upload){if(path().empty()||!p)return;std::lock_guard<std::mutex> lock(mutex);if(resources.size()>=8192)return;
 auto old=resources.find(p);if(old!=resources.end())log("\"event\":\"invalidate\",\"resource\":"+std::to_string(old->second.id)+",\"reason\":\"identity_recreated\"",true);
 Resource r;r.id=++nextId;r.generation=1;r.bytes=p->GetDesc().Width;r.mapped=mapped;r.upload=upload;resources[p]=r;
 log("\"event\":\"create\",\"resource\":"+std::to_string(r.id)+",\"native_identity\":"+std::to_string((uintptr_t)p)+",\"generation\":1,\"bytes\":"+std::to_string(r.bytes)+",\"upload\":"+(upload?"true":"false"));
}
inline void destroyed(ID3D12Resource* p){if(path().empty())return;std::lock_guard<std::mutex> lock(mutex);auto it=resources.find(p);if(it==resources.end())return;log("\"event\":\"invalidate\",\"resource\":"+std::to_string(it->second.id)+",\"generation\":"+std::to_string(it->second.generation)+",\"reason\":\"destroyed\"",it->second.selected);resources.erase(it);}
inline void begin(ID3D12CommandList* list){if(path().empty())return;std::lock_guard<std::mutex> lock(mutex);if(recordings.size()>=4096&&recordings.find(list)==recordings.end())return;recordings[list]=++nextId;}
inline void used(ID3D12Resource* p){if(path().empty()||!p)return;std::lock_guard<std::mutex> lock(mutex);auto i=resources.find(p);if(i!=resources.end())++i->second.bound;}
inline void barriers(const D3D12_RESOURCE_BARRIER* b,size_t n){if(path().empty())return;std::lock_guard<std::mutex> lock(mutex);for(size_t k=0;k<n;++k){
 if(b[k].Type==D3D12_RESOURCE_BARRIER_TYPE_TRANSITION){auto i=resources.find(b[k].Transition.pResource);if(i!=resources.end()&&(b[k].Transition.StateAfter&D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE))++i->second.readable;}
 if(b[k].Type==D3D12_RESOURCE_BARRIER_TYPE_ALIASING){if(!b[k].Aliasing.pResourceBefore||!b[k].Aliasing.pResourceAfter){for(auto& item:resources){auto& r=item.second;if(!r.selected)continue;log("\"event\":\"invalidate\",\"resource\":"+std::to_string(r.id)+",\"generation\":"+std::to_string(r.generation)+",\"reason\":\"unknown_alias_scope\"",true);++r.generation;r.pending=false;r.captures=0;}continue;}for(auto p:{b[k].Aliasing.pResourceBefore,b[k].Aliasing.pResourceAfter}){auto i=resources.find(p);if(i==resources.end())continue;auto& r=i->second;log("\"event\":\"invalidate\",\"resource\":"+std::to_string(r.id)+",\"generation\":"+std::to_string(r.generation)+",\"reason\":\"aliasing\"",true);++r.generation;r.pending=false;r.captures=0;}}
 }}
inline void pollLocked(){for(auto& s:snapshots){if(!s.submitted||s.complete||s.invalid)continue;Queue* q=nullptr;for(auto& item:queues)if(item.second.id==s.queue)q=&item.second;if(!q)continue;auto completed=q->fence->GetCompletedValue();if(completed==~uint64_t(0)){s.invalid=true;log("\"event\":\"invalid_snapshot\",\"snapshot\":"+std::to_string(s.id)+",\"reason\":\"device_removed\"",true);continue;}if(completed<s.fence)continue;
 void* bytes{};D3D12_RANGE range{0,size_t(s.bytes)};check(s.readback->Map(0,&range,&bytes));std::ofstream f(path()+"/s"+std::to_string(s.id)+".gpu.bin",std::ios::binary);f.write((char*)bytes,s.bytes);s.readback->Unmap(0,nullptr);if(!f)throw std::runtime_error("snapshot write");s.complete=true;
 for(auto& item:resources)if(item.second.id==s.resource&&item.second.generation==s.generation){item.second.pending=false;++item.second.captures;}
 log("\"event\":\"completed\",\"snapshot\":"+std::to_string(s.id)+",\"resource\":"+std::to_string(s.resource)+",\"generation\":"+std::to_string(s.generation)+",\"write_op\":"+std::to_string(s.write)+",\"queue\":"+std::to_string(s.queue)+",\"fence_value\":"+std::to_string(s.fence)+",\"completed_value\":"+std::to_string(q->fence->GetCompletedValue()),true);
 }}
inline void selectLocked(){if(selected>=2||std::chrono::duration<double>(Clock::now()-start).count()<5)return;
 std::vector<Resource*> rank;for(auto& item:resources){auto& r=item.second;if(!r.upload&&!r.selected&&r.copies>=3&&r.bytes>=4096&&(r.readable||r.bound))rank.push_back(&r);}
 std::sort(rank.begin(),rank.end(),[](auto a,auto b){return a->copied>b->copied;});for(auto* r:rank){if(selected==2)break;r->selected=true;++selected;log("\"event\":\"selected\",\"resource\":"+std::to_string(r->id)+",\"generation\":"+std::to_string(r->generation)+",\"copy_operations\":"+std::to_string(r->copies)+",\"copied_bytes\":"+std::to_string(r->copied)+",\"shader_readable_transitions\":"+std::to_string(r->readable)+",\"explicit_bind_events\":"+std::to_string(r->bound)+",\"reason\":\"rank_recurring_upload_writes_and_read_evidence\"",true);}
}
inline void copy(ID3D12GraphicsCommandList* list,ID3D12Resource* dst,uint64_t offset,ID3D12Resource* src,uint64_t srcOffset,uint64_t bytes){if(path().empty())return;std::lock_guard<std::mutex> lock(mutex);pollLocked();auto d=resources.find(dst),u=resources.find(src);if(d==resources.end()||u==resources.end())return;auto& r=d->second;if(u->second.upload&&u->second.mapped){++r.copies;r.copied+=bytes;}uint64_t write=++operation;
 if(!r.selected||r.pending||r.captures>=3||snapshots.size()>=6||!u->second.upload||!u->second.mapped||r.upload||bytes<4096||offset>r.bytes||bytes>r.bytes-offset)return;
 auto recorded=recordings.find(list);if(recorded==recordings.end())return;
 Snapshot s;s.id=snapshots.size();s.resource=r.id;s.generation=r.generation;s.write=write;s.range=offset;s.bytes=std::min<uint64_t>(bytes,128*1024);s.list=(uintptr_t)list;s.recording=recorded->second;s.source=dst;
 std::vector<const void*> excluded;for(auto& item:resources)if(item.second.mapped)excluded.push_back(item.second.mapped);
 arc_memory_windows::capture(path()+"/s"+std::to_string(s.id),write,excluded);
 ComPtr<ID3D12Device> dev;check(dst->GetDevice(IID_PPV_ARGS(&dev)));D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=s.bytes;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;check(dev->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&s.readback)));
 D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={dst,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&b);list->CopyBufferRegion(s.readback.Get(),0,dst,offset,s.bytes);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);
 log("\"event\":\"snapshot_recorded\",\"snapshot\":"+std::to_string(s.id)+",\"resource\":"+std::to_string(r.id)+",\"generation\":"+std::to_string(r.generation)+",\"write_op\":"+std::to_string(write)+",\"write_kind\":\"CopyBufferRegion\",\"source_resource\":"+std::to_string(u->second.id)+",\"source_generation\":"+std::to_string(u->second.generation)+",\"source_offset\":"+std::to_string(srcOffset)+",\"write_offset\":"+std::to_string(offset)+",\"write_bytes\":"+std::to_string(bytes)+",\"snapshot_bytes\":"+std::to_string(s.bytes)+",\"command_list\":"+std::to_string(s.list)+",\"recording\":"+std::to_string(s.recording),true);r.pending=true;snapshots.push_back(std::move(s));
}
inline void submitted(ID3D12CommandQueue* queue,ID3D12CommandList* const* lists,size_t count){if(path().empty())return;std::lock_guard<std::mutex> lock(mutex);pollLocked();selectLocked();Queue* q=nullptr;
 for(auto& s:snapshots){bool present=false;for(size_t i=0;i<count;++i)if((uintptr_t)lists[i]==s.list&&recordings[lists[i]]==s.recording)present=true;if(!present)continue;
  if(s.submitted){s.invalid=true;log("\"event\":\"invalid_snapshot\",\"snapshot\":"+std::to_string(s.id)+",\"reason\":\"repeated_command_execution\"",true);continue;}
  if(!q){auto& own=queues[queue];q=&own;if(!q->fence){q->id=++nextId;ComPtr<ID3D12Device> dev;check(queue->GetDevice(IID_PPV_ARGS(&dev)));check(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&q->fence)));}++q->value;check(queue->Signal(q->fence.Get(),q->value));}
  s.submitted=true;s.queue=q->id;s.fence=q->value;log("\"event\":\"snapshot_submitted\",\"snapshot\":"+std::to_string(s.id)+",\"queue\":"+std::to_string(s.queue)+",\"fence_value\":"+std::to_string(s.fence)+",\"recording\":"+std::to_string(s.recording),true);
 }
}
}

#pragma once
#include "arc/gpu_attribution.hpp"
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <wrl/client.h>

namespace arc_wicked {
// Diagnostic single-frame capture. Existing semantic counters are untouched.
// The current backend does not expose complete shader descriptor indexing or
// queue waits here: those remain explicitly unresolved in exported dependencies.
class AttributionCapture {
    struct CpuScope {
        std::atomic<std::uint64_t>& sum;
        std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
        ~CpuScope(){sum.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count()),std::memory_order_relaxed);}
    };
    std::atomic<std::uint64_t> capture_cpu_ns_{};
    struct State {
        std::map<arc::ResourceId,unsigned> candidates;
        arc::RasterRegion raster;
        bool viewport{},scissor{},targets{};
        std::size_t works{};
        int open_query{-1};
        std::vector<std::pair<std::size_t,unsigned>> samples;
    };
    struct QueueTiming {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        std::uint64_t value{},frequency{};
    };
    struct Sample {arc::WorkId work{};arc::QueueId queue{};unsigned query{};std::uint64_t begin{},end{};};
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> query_heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> query_readback_;
    std::map<arc::QueueId,QueueTiming> queue_timings_;
    std::vector<Sample> samples_;
    unsigned next_query_{};
    bool pending_export_{},present_success_{};
    arc::WorkId present_work_{};
    std::filesystem::path path_;
    std::string source_;
    std::mutex mutex_;
    std::map<arc::CommandId,State> states_;
    arc::GpuAttributionGraph graph_;
    std::atomic<bool> active_{false};
    std::atomic<bool> requested_{false},started_{false};
    unsigned frames_{};
    unsigned capture_frame_{};
public:
    void configure(ID3D12Device* device,std::filesystem::path path,std::string source){
        path_=std::move(path);source_=std::move(source);
        if(path_.empty())return;
        device_=device;
        D3D12_QUERY_HEAP_DESC hd{};hd.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;hd.Count=128;
        if(FAILED(device_->CreateQueryHeap(&hd,IID_PPV_ARGS(&query_heap_))))return;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=128*sizeof(std::uint64_t);
        rd.Height=1;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(device_->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&query_readback_))))query_heap_.Reset();
    }
    bool active()const noexcept{return active_.load(std::memory_order_relaxed);}
    bool enabled()const noexcept{return !path_.empty();}
    void arm(){if(enabled()&&!started_.exchange(true))requested_.store(true);}
    void begin(arc::CommandId command){
        if(!active())return;CpuScope cpu{capture_cpu_ns_};std::scoped_lock lock(mutex_);
        if(graph_.begin(command))states_[command]={};
    }
    void use(arc::CommandId command,arc::ResourceId resource,bool write){
        if(!active()||!resource)return;CpuScope cpu{capture_cpu_ns_};std::scoped_lock lock(mutex_);
        auto it=states_.find(command);if(it==states_.end())return;
        auto& candidates=it->second.candidates;
        if(candidates.size()>=64&&!candidates.contains(resource)){
            // Force fail-closed instead of silently dropping a binding candidate.
            arc::WorkObservation invalid;invalid.accesses.resize(65);graph_.record(command,invalid);return;
        }
        candidates[resource]|=write?2:1;
    }
    void region(arc::CommandId command,unsigned kind,double x,double y,double w,double h){
        if(!active())return;CpuScope cpu{capture_cpu_ns_};std::scoped_lock lock(mutex_);auto it=states_.find(command);if(it==states_.end())return;
        auto& s=it->second;
        if(kind==0){s.raster.viewport={x,y,w,h};s.viewport=true;}
        if(kind==1){s.raster.scissor={x,y,w,h};s.scissor=true;}
        if(kind==2){s.raster.width=static_cast<unsigned>(w);s.raster.height=static_cast<unsigned>(h);s.targets=true;}
        if(kind==3){s.targets=false;s.raster.width=0;s.raster.height=0;}
        if(kind==4)s.viewport=false;
        if(kind==5)s.scissor=false;
        s.raster.known=s.viewport&&s.scissor&&s.targets;
    }
    void count(arc::CommandId command,ID3D12CommandList* native,unsigned kind,std::uint64_t items){
        if(!active())return;CpuScope cpu{capture_cpu_ns_};std::scoped_lock lock(mutex_);auto it=states_.find(command);if(it==states_.end())return;
        arc::WorkObservation w;w.kind=kind<2?arc::GpuWorkKind::Draw:kind==2?arc::GpuWorkKind::Dispatch:arc::GpuWorkKind::Indirect;
        w.items=items;w.raster=it->second.raster;
        // Cumulative candidates since Reset are a conservative superset, not current bindings.
        // Even a bound RTV/UAV does not prove that this draw wrote a pixel.
        for(auto [id,mask]:it->second.candidates){
            if(mask&1)w.accesses.push_back({id,false,arc::AccessEvidence::Possible,false});
            if(mask&2)w.accesses.push_back({id,true,arc::AccessEvidence::Possible,false});
        }
        if(graph_.record(command,w)){
            // Fixed global budget; indirect work is left unmeasured in this first adapter.
            if(query_heap_&&kind<3&&next_query_<64){
                Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
                if(SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&list)))){
                    unsigned query=next_query_++;it->second.open_query=static_cast<int>(query);
                    it->second.samples.emplace_back(it->second.works,query);
                    list->EndQuery(query_heap_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query*2);
                }
            }
            ++it->second.works;
        }
    }
    void end_work(arc::CommandId command,ID3D12GraphicsCommandList* list){
        if(!active())return;CpuScope cpu{capture_cpu_ns_};std::scoped_lock lock(mutex_);auto it=states_.find(command);if(it==states_.end())return;
        if(it->second.open_query>=0){list->EndQuery(query_heap_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,static_cast<unsigned>(it->second.open_query)*2+1);it->second.open_query=-1;}
    }
    void resolve(arc::CommandId command,ID3D12GraphicsCommandList* list){
        if(!active())return;CpuScope cpu{capture_cpu_ns_};std::scoped_lock lock(mutex_);auto it=states_.find(command);if(it==states_.end())return;
        if(it->second.open_query>=0){graph_.invalidate();return;}
        for(auto [work,query]:it->second.samples){(void)work;list->ResolveQueryData(query_heap_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,query*2,2,query_readback_.Get(),query*2*sizeof(std::uint64_t));}
    }
    void copy(arc::CommandId command,arc::ResourceId src,arc::ResourceId dst,std::uint64_t bytes){
        if(!active())return;CpuScope cpu{capture_cpu_ns_};std::scoped_lock lock(mutex_);if(!states_.contains(command))return;
        arc::WorkObservation w;w.kind=arc::GpuWorkKind::Copy;w.bindings_complete=true;w.copy_bytes=bytes;
        w.accesses={{src,false,arc::AccessEvidence::Observed,false},{dst,true,arc::AccessEvidence::Observed,false}};
        if(graph_.record(command,w))++states_.at(command).works;
    }
    void submit(arc::QueueId queue,ID3D12CommandQueue* native,arc::CommandId command){
        if(!active())return;CpuScope cpu{capture_cpu_ns_};std::scoped_lock lock(mutex_);
        if(!states_.contains(command)){graph_.invalidate();return;}graph_.close(command);auto ids=graph_.submit(queue,command);
        const auto& state=states_.at(command);
        if(!state.samples.empty()){
            auto& qt=queue_timings_[queue];
            if(!qt.fence){
                if(FAILED(device_->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&qt.fence)))||FAILED(native->GetTimestampFrequency(&qt.frequency)))graph_.invalidate();
            }
            if(qt.fence&&SUCCEEDED(native->Signal(qt.fence.Get(),++qt.value))){
                for(auto [work,query]:state.samples)if(work<ids.size())samples_.push_back({ids[work],queue,query});else graph_.invalidate();
            }else graph_.invalidate();
        }
        graph_.retire_command(command);states_.erase(command);
    }
    void presented(arc::QueueId queue,arc::ResourceId backbuffer,bool success){
        if(path_.empty())return;std::scoped_lock lock(mutex_);++frames_;
        if(requested_.exchange(false)){graph_.clear();states_.clear();capture_frame_=frames_+1;active_.store(true,std::memory_order_relaxed);return;}
        if(active()){
            active_.store(false,std::memory_order_relaxed);
            present_success_=success;present_work_=success?graph_.present(queue,backbuffer):0;pending_export_=true;
        }
        if(!pending_export_)return;
        bool ready=true;for(const auto& [id,qt]:queue_timings_){(void)id;if(!qt.fence||qt.fence->GetCompletedValue()<qt.value||qt.fence->GetCompletedValue()==UINT64_MAX)ready=false;}
        if(!ready&&frames_<capture_frame_+300)return;
        if(!ready)graph_.invalidate();
        if(ready&&!samples_.empty()){
            void* data=nullptr;D3D12_RANGE range{0,128*sizeof(std::uint64_t)};
            if(SUCCEEDED(query_readback_->Map(0,&range,&data))){
                const auto* ticks=static_cast<const std::uint64_t*>(data);
                for(auto& sample:samples_){sample.begin=ticks[sample.query*2];sample.end=ticks[sample.query*2+1];graph_.timing(sample.work,{sample.begin,sample.end,queue_timings_.at(sample.queue).frequency,true});}
                D3D12_RANGE empty{0,0};query_readback_->Unmap(0,&empty);
            }else graph_.invalidate();
        }
        auto slice=graph_.ancestors(present_work_);
        if(!path_.parent_path().empty())std::filesystem::create_directories(path_.parent_path());
        std::ofstream out(path_);
        out<<"{\"schema\":1,\"source_sha\":\""<<source_<<"\",\"scope\":\"sampled_frame_candidate_bindings\","
              "\"candidate_scope\":\"cumulative_since_command_reset\",\"shader_access_complete\":false,"
              "\"queue_fences_observed\":false,\"timing_scope\":\"first_64_direct_draw_dispatch_in_sampled_frame\","
              "\"present_succeeded\":"<<(present_success_?"true":"false")<<",\"frame\":"<<capture_frame_<<",\"export_frame\":"<<frames_
           <<",\"present_work\":"<<present_work_<<",\"present_slice_complete\":"<<(slice.complete?"true":"false")
           <<",\"capture_callback_thread_ms\":"<<double(capture_cpu_ns_.load())/1000000.0
           <<",\"present_ancestors\":[";
        for(std::size_t i=0;i<slice.contributors.size();++i){if(i)out<<',';out<<slice.contributors[i];}
        out<<"],\"timestamp_samples\":[";
        for(std::size_t i=0;i<samples_.size();++i){if(i)out<<',';const auto& s=samples_[i];out<<"{\"work\":"<<s.work<<",\"queue\":"<<s.queue<<",\"begin\":"<<s.begin<<",\"end\":"<<s.end<<",\"frequency\":"<<queue_timings_.at(s.queue).frequency<<'}';}
        out<<"],\"graph\":";graph_.write_json(out);out<<"}\n";
        pending_export_=false;states_.clear();graph_.clear();
    }
};
} // namespace arc_wicked

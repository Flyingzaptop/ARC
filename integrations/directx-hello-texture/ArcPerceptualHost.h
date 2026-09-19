#pragma once
#include "arc/perceptual_trial.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <cmath>
#include <iomanip>
#include <stdexcept>

// Adapter for Microsoft's raster sample. No policy/critic implementation here.
class ArcPerceptualHost final:public arc::PerceptualProbeHost {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Device> device_;Ptr<ID3D12CommandQueue> queue_;Ptr<ID3D12Resource> texture_;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_{};D3D12_SHADER_RESOURCE_VIEW_DESC original_{};
    Ptr<ID3D12QueryHeap> queries_;Ptr<ID3D12Resource> times_,pixels_;
    Ptr<ID3D12CommandAllocator> allocator_;Ptr<ID3D12GraphicsCommandList> copy_;
    Ptr<ID3D12Fence> fence_;UINT64 fence_value_{},frequency_{};HANDLE event_{};
    std::function<ID3D12Resource*()> render_;std::filesystem::path output_;
    bool modified_{};unsigned probe_{};
    static void check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Microsoft perceptual adapter HRESULT");}
    Ptr<ID3D12Resource> readback(UINT64 bytes){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC d{};
        d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Ptr<ID3D12Resource> out;check(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out)));return out;
    }
    void idle(){
        check(queue_->Signal(fence_.Get(),++fence_value_));check(fence_->SetEventOnCompletion(fence_value_,event_));
        if(WaitForSingleObject(event_,30000)!=WAIT_OBJECT_0)throw std::runtime_error("external GPU timeout");check(device_->GetDeviceRemovedReason());
    }
public:
    ArcPerceptualHost(ID3D12Device* device,ID3D12CommandQueue* queue,ID3D12Resource* texture,
        D3D12_CPU_DESCRIPTOR_HANDLE srv,std::function<ID3D12Resource*()> render,std::filesystem::path output)
        :device_(device),queue_(queue),texture_(texture),srv_(srv),render_(std::move(render)),output_(std::move(output)){
        std::filesystem::create_directories(output_);
        original_.Format=DXGI_FORMAT_R8G8B8A8_UNORM;original_.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;original_.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;original_.Texture2D.MipLevels=2;
        D3D12_QUERY_HEAP_DESC q{};q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=2;check(device_->CreateQueryHeap(&q,IID_PPV_ARGS(&queries_)));
        times_=readback(16);check(queue_->GetTimestampFrequency(&frequency_));check(device_->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence_)));
        event_=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event_)throw std::runtime_error("event");
        check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator_)));
        check(device_->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator_.Get(),nullptr,IID_PPV_ARGS(&copy_)));check(copy_->Close());
    }
    ~ArcPerceptualHost(){if(event_)CloseHandle(event_);}
    void begin_frame(ID3D12GraphicsCommandList* list){list->EndQuery(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);}
    void end_frame(ID3D12GraphicsCommandList* list){list->EndQuery(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);list->ResolveQueryData(queries_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,times_.Get(),0);}
    bool prepare(const arc::PerceptualCapability& c)override {++probe_;return c.target==1&&c.generation==1&&!modified_&&texture_->GetDesc().MipLevels==2;}
    bool apply(const arc::PerceptualCapability& c)override {
        if(c.target!=1||c.generation!=1)return false;idle();auto view=original_;view.Texture2D.MostDetailedMip=1;view.Texture2D.MipLevels=1;device_->CreateShaderResourceView(texture_.Get(),&view,srv_);modified_=true;return true;
    }
    bool restore(const arc::PerceptualCapability& c)noexcept override {
        try{if(c.target!=1||c.generation!=1)return false;idle();device_->CreateShaderResourceView(texture_.Get(),&original_,srv_);modified_=false;return true;}catch(...){return false;}
    }
    void finish()noexcept override{}
    bool restored()const noexcept{return !modified_;}
    std::optional<arc::ProbeCapture> capture(arc::ProbePhase phase)override {
        arc::ProbeCapture p;p.state_key=1;p.generation=1;p.linear_rgb=true;
        ID3D12Resource* target=nullptr;std::vector<UINT64> raw;
        for(int i=0;i<11;++i){
            target=render_();idle();void* data{};D3D12_RANGE range{0,16};check(times_->Map(0,&range,&data));const auto* ticks=static_cast<const UINT64*>(data);const UINT64 start=ticks[0],end=ticks[1];D3D12_RANGE empty{0,0};times_->Unmap(0,&empty);
            if(end<start)throw std::runtime_error("query order");raw.push_back(start);raw.push_back(end);if(i>=2)p.gpu_ms.push_back(double(end-start)*1000/double(frequency_));
        }
        if(!target)return {};const auto desc=target->GetDesc();if(desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM||desc.SampleDesc.Count!=1)return {};
        p.width=static_cast<UINT>(desc.Width);p.height=desc.Height;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};device_->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);pixels_=readback(bytes);
        check(allocator_->Reset());check(copy_->Reset(allocator_.Get(),nullptr));D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={target,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE};copy_->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=target;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=pixels_.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;copy_->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);copy_->ResourceBarrier(1,&barrier);check(copy_->Close());ID3D12CommandList* lists[]{copy_.Get()};queue_->ExecuteCommandLists(1,lists);idle();
        void* data{};check(pixels_->Map(0,nullptr,&data));p.rgb.resize(std::size_t(p.width)*p.height*3);
        for(UINT y=0;y<p.height;++y){const auto* row=static_cast<const unsigned char*>(data)+footprint.Offset+UINT64(y)*footprint.Footprint.RowPitch;for(UINT x=0;x<p.width;++x)for(UINT k=0;k<3;++k)p.rgb[(std::size_t(y)*p.width+x)*3+k]=float(row[x*4+k])/255;}
        D3D12_RANGE empty{0,0};pixels_->Unmap(0,&empty);p.readback_complete=true;
        const auto stem=std::to_string(probe_)+"-"+std::to_string(int(phase));std::ofstream bin(output_/(stem+".rgb32f"),std::ios::binary);bin.write(reinterpret_cast<const char*>(p.rgb.data()),static_cast<std::streamsize>(p.rgb.size()*sizeof(float)));
        std::ofstream meta(output_/(stem+".json"));meta<<std::setprecision(17)<<"{\"width\":"<<p.width<<",\"height\":"<<p.height<<",\"state_key\":1,\"generation\":1,\"mip\":"<<(modified_?1:0)<<",\"frequency\":"<<frequency_<<",\"ticks\":[";for(std::size_t i=0;i<raw.size();++i){if(i)meta<<',';meta<<raw[i];}meta<<"],\"gpu_ms\":[";for(std::size_t i=0;i<p.gpu_ms.size();++i){if(i)meta<<',';meta<<p.gpu_ms[i];}meta<<"]}";
        return p;
    }
    int run(){
        const auto reference=capture(arc::ProbePhase::ReferenceBefore);if(!reference)return 2;
        std::size_t affected=0;for(std::size_t i=0;i<reference->rgb.size();i+=3)if(std::abs(reference->rgb[i])>.01||std::abs(reference->rgb[i+1]-.2)>.01||std::abs(reference->rgb[i+2]-.4)>.01)++affected;
        arc::TemporalVisibilityModel visibility;arc::VisibilityObservation observation;observation.id=1;observation.frame=1;observation.visible_coverage=double(affected)/(reference->width*reference->height);observation.local_coverage_upper=observation.visible_coverage;observation.present_reachable=true;observation.confidence=1;
        if(!visibility.observe(observation))return 3;
        arc::PerceptualCandidate candidate;candidate.capability={1,1,1,arc::PerceptualMechanism::SrvMipRange,true,true,true,true};candidate.importance=arc::VisualImportanceModel{}.evaluate(*visibility.find(1));auto samples=reference->gpu_ms;std::sort(samples.begin(),samples.end());candidate.measured_cost_ms=samples[samples.size()/2];candidate.expected_gain_ms=candidate.measured_cost_ms*.25;
        arc::PerceptualTrialController controller;const auto result=controller.trial(*this,candidate);const bool rollback=controller.restore(*this)&&restored();
        std::ofstream summary(output_/"summary.json");summary<<std::setprecision(17)<<"{\"renderer\":\"Microsoft D3D12HelloTexture\",\"status\":"<<int(result.status)<<",\"reason\":"<<int(result.verdict.reason)<<",\"importance\":"<<candidate.importance.score<<",\"gain_ms\":"<<result.verdict.gain_ms<<",\"image_mean\":"<<result.verdict.modified_mean<<",\"image_peak\":"<<result.verdict.modified_peak<<",\"restored\":"<<(rollback?"true":"false")<<'}';
        // A cheap external workload should abstain when no useful gain exists.
        // Portability is not a license to lower the core's acceptance thresholds.
        return rollback&&(result.status==arc::TrialStatus::Retained||(result.status==arc::TrialStatus::Rejected&&result.verdict.reason!=arc::CriticReason::InvalidCapture&&result.verdict.reason!=arc::CriticReason::InvalidTiming))?0:4;
    }
};

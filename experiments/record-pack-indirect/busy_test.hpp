#pragma once
// Diagnostic only: a real delayed GPU consumer, not a forged completion value.
namespace arc_indirect {
inline bool busyEnabled(){static bool v=GetEnvironmentVariableA("ARC_INDIRECT_BUSY_TEST",nullptr,0)!=0;return v;}
struct HeldVersion {uint64_t frame{};ComPtr<ID3D12CommandAllocator> alloc;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Resource> readback;std::vector<unsigned char> expected;bool verified{};};
inline ComPtr<ID3D12CommandQueue> delayed;
inline ComPtr<ID3D12Fence> graphicsDone,gate;
inline HeldVersion held[3];inline uint32_t heldCount{};inline bool holding{},released{};inline Clock::time_point holdStart;
inline void busyPulse(){if(!busyEnabled()||!holding)return;
 if(!released&&((heldCount==3&&elapsed(holdStart)>400)||elapsed(holdStart)>2000)){check(gate->Signal(1));released=true;ARCWickedCpuSample("Busy test held versions",heldCount);ARCWickedCpuSample("Busy test GPU hold ms",elapsed(holdStart));if(heldCount!=3)throw std::runtime_error("did not fill three GPU versions");}
 if(!released)return;
 for(uint32_t i=0;i<heldCount;++i){auto& h=held[i];if(h.verified||consumers->GetCompletedValue()<h.frame+1)continue;void* p{};check(h.readback->Map(0,nullptr,&p));bool equal=memcmp(p,h.expected.data(),h.expected.size())==0;h.readback->Unmap(0,nullptr);if(!equal)throw std::runtime_error("busy GPU consumer saw overwritten version");h.verified=true;ARCWickedCpuSample("Busy test verified version",1);ARCWickedCpuSample("Busy test verified bytes",double(h.expected.size()));}
}
inline void busyRetire(ID3D12CommandQueue* q,uint64_t frame){if(!busyEnabled()){check(q->Signal(consumers.Get(),frame+1));return;}
 auto* dev=static_cast<GraphicsDevice_DX12*>(GetDevice());auto* d=dev->ArcPacketDevice();
 if(!delayed){D3D12_COMMAND_QUEUE_DESC desc{};check(d->CreateCommandQueue(&desc,IID_PPV_ARGS(&delayed)));check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&graphicsDone)));check(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));}
 bool candidates=mode()==2&&frames[frame%3].frame==frame;for(auto& s:slots[frame%3])candidates=candidates&&s.frame==frame&&s.pending&&s.commands>0;
 if(!holding&&candidates){holding=true;holdStart=Clock::now();check(delayed->Wait(gate.Get(),1));}
 check(q->Signal(graphicsDone.Get(),frame+1));check(delayed->Wait(graphicsDone.Get(),frame+1));
 if(holding&&!released&&heldCount<3&&candidates){auto& h=held[heldCount++];h.frame=frame;check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&h.alloc)));check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,h.alloc.Get(),nullptr,IID_PPV_ARGS(&h.list)));
  struct Copy{ID3D12Resource* resource;D3D12_RESOURCE_STATES state;size_t offset,bytes;};std::vector<Copy> copies;
  auto add=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES state,const void* bytes,size_t n){copies.push_back({r,state,h.expected.size(),n});auto* b=(const unsigned char*)bytes;h.expected.insert(h.expected.end(),b,b+n);};
  add(frames[frame%3].extra.Get(),D3D12_RESOURCE_STATE_GENERIC_READ,frames[frame%3].extraMapped,side.size()*sizeof(Extra));
  for(auto& s:slots[frame%3]){add(s.worker.upload.Get(),D3D12_RESOURCE_STATE_GENERIC_READ,s.worker.mapped,s.records*16);add(s.worker.templateUpload.Get(),D3D12_RESOURCE_STATE_GENERIC_READ,s.templates.data(),s.templates.size()*4);add(dev->ArcPacketResource(&s.output),D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,s.expected.data(),s.expected.size()*4);auto args=s.templates;for(uint32_t j=0;j<s.drawCount;++j){args[j*5+1]=uint32_t(s.expected.size());args[j*5+4]=0;}add(dev->ArcPacketResource(&s.args),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,args.data(),args.size()*4);}
  Worker helper;helper.device=d;h.readback=helper.buffer(h.expected.size(),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  for(auto& c:copies){if(c.state!=D3D12_RESOURCE_STATE_GENERIC_READ)transition(h.list.Get(),c.resource,c.state,D3D12_RESOURCE_STATE_COPY_SOURCE);h.list->CopyBufferRegion(h.readback.Get(),c.offset,c.resource,0,c.bytes);if(c.state!=D3D12_RESOURCE_STATE_GENERIC_READ)transition(h.list.Get(),c.resource,D3D12_RESOURCE_STATE_COPY_SOURCE,c.state);}check(h.list->Close());ID3D12CommandList* list=h.list.Get();delayed->ExecuteCommandLists(1,&list);
 }
 check(delayed->Signal(consumers.Get(),frame+1));
}
}

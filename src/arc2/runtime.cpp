#include "arc/arc2/runtime.hpp"
#include <algorithm>
#include <limits>

namespace arc::arc2 {
Runtime::Runtime(std::size_t max_work, std::size_t max_history) : max_work_(max_work), max_history_(max_history) {}
bool Runtime::valid(ObjectId id, ObjectKind kind) const { auto it=objects_.find(id); return it!=objects_.end() && it->second.alive && it->second.kind==kind; }
void Runtime::touch(ObjectId id) { auto& l=lists_[id]; ++l.sequence; l.last_was_clear=false; }
ObjectId Runtime::create_object(ObjectKind kind, std::uintptr_t native_identity) {
    std::lock_guard lock(mutex_);
    if (native_identity) { auto it=native_.find(native_identity); if(it!=native_.end() && objects_.at(it->second).alive) { incomplete_=true; return {}; } }
    ObjectId id{next_object_++}; objects_[id]={id,kind,native_identity,true};
    if(native_identity) native_[native_identity]=id;
    if(kind==ObjectKind::CommandList) lists_[id]={};
    return id;
}
void Runtime::destroy_object(ObjectId id) { std::lock_guard lock(mutex_); auto it=objects_.find(id); if(it==objects_.end()||!it->second.alive) return; it->second.alive=false; if(it->second.native_identity) native_.erase(it->second.native_identity); if(it->second.kind==ObjectKind::CommandList) lists_.erase(id); if(it->second.kind==ObjectKind::DescriptorHeap) for(auto s=slots_.begin();s!=slots_.end();) s=s->first.heap==id?slots_.erase(s):++s; }
ObjectId Runtime::find_object(std::uintptr_t native_identity) const { std::lock_guard lock(mutex_); auto it=native_.find(native_identity); return it==native_.end()?ObjectId{}:it->second; }
void Runtime::describe_resource(ObjectId id,std::uint64_t bytes,ObjectId heap,std::uint64_t offset) { std::lock_guard lock(mutex_); if(valid(id,ObjectKind::Resource)) { auto& r=resources_[id]; r.bytes=bytes; r.heap=heap; r.offset=offset; } else incomplete_=true; }
void Runtime::describe_resource_shape(ObjectId id,ResourceShape shape) { std::lock_guard lock(mutex_); if(valid(id,ObjectKind::Resource)) resources_[id].shape=shape; else incomplete_=true; }
void Runtime::describe_shader(ObjectId shader,std::span<const Access> declared) { std::lock_guard lock(mutex_); if(valid(shader,ObjectKind::Shader)) shaders_[shader]={declared.begin(),declared.end()}; else incomplete_=true; }
void Runtime::describe_shader_bindings(ObjectId shader,std::span<const ShaderBinding> bindings) { std::lock_guard lock(mutex_); if(valid(shader,ObjectKind::Shader)) shader_bindings_[shader]={bindings.begin(),bindings.end()}; else incomplete_=true; }
void Runtime::describe_root_signature(ObjectId root,std::span<const RootParameter> parameters) { std::lock_guard lock(mutex_); if(valid(root,ObjectKind::RootSignature)) root_parameters_[root]={parameters.begin(),parameters.end()}; else incomplete_=true; }
void Runtime::describe_descriptor_heap(ObjectId heap,std::uint32_t count) { std::lock_guard lock(mutex_); if(valid(heap,ObjectKind::DescriptorHeap)) descriptor_heap_counts_[heap]=count; else incomplete_=true; }
void Runtime::set_pipeline_shaders(ObjectId pso,std::span<const ObjectId> shaders) { std::lock_guard lock(mutex_); if(valid(pso,ObjectKind::PipelineState)) pipeline_shaders_[pso]={shaders.begin(),shaders.end()}; else incomplete_=true; }
void Runtime::describe_pipeline_fixed(ObjectId pso,FixedPipelineState state) { std::lock_guard lock(mutex_); if(valid(pso,ObjectKind::PipelineState)) pipeline_fixed_[pso]=state; else incomplete_=true; }
DescriptorRef Runtime::write_descriptor(ObjectId heap,std::uint32_t index,ObjectId resource,ViewKind kind) { std::lock_guard lock(mutex_); if(!valid(heap,ObjectKind::DescriptorHeap)||(descriptor_heap_counts_.contains(heap)&&index>=descriptor_heap_counts_.at(heap))) { incomplete_=true; return {}; } auto& slot=slots_[{heap,index}]; DescriptorRef ref{heap,index,++slot.generation}; slot.current={ref,resource,kind,true}; if(resource && !valid(resource,ObjectKind::Resource)) incomplete_=true; return ref; }
DescriptorRef Runtime::copy_descriptor(ObjectId heap,std::uint32_t index,DescriptorRef source) { std::lock_guard lock(mutex_); if(!valid(heap,ObjectKind::DescriptorHeap)||(descriptor_heap_counts_.contains(heap)&&index>=descriptor_heap_counts_.at(heap))) { incomplete_=true; return {}; } auto src=slots_.find({source.heap,source.index}); Descriptor data{}; if(src!=slots_.end() && src->second.generation==source.generation) data=src->second.current; else incomplete_=true; auto& slot=slots_[{heap,index}]; DescriptorRef ref{heap,index,++slot.generation}; slot.current={ref,data.resource,data.kind,data.valid}; return ref; }
DescriptorRef Runtime::descriptor(ObjectId heap,std::uint32_t index) const { std::lock_guard lock(mutex_); auto it=slots_.find({heap,index}); return it==slots_.end()?DescriptorRef{}:it->second.current.ref; }
void Runtime::reset_command_list(ObjectId id,ObjectId allocator,ObjectId pso) { std::lock_guard lock(mutex_); if(!valid(id,ObjectKind::CommandList)) { incomplete_=true; return; } auto& list=lists_[id]; ++list.generation; ++list.sequence; list.last_was_clear=false; list.poisoned=false; list.allocator=allocator; list.closed=false; list.state={}; list.state.pipeline=pso; list.work.clear(); list.recorded.clear(); }
void Runtime::close_command_list(ObjectId id) { std::lock_guard lock(mutex_); auto it=lists_.find(id); if(it!=lists_.end()) it->second.closed=true; else incomplete_=true; }
void Runtime::set_pipeline(ObjectId id,ObjectId pso) { std::lock_guard lock(mutex_); touch(id); auto& s=lists_[id].state; s.pipeline=pso; s.shaders=pipeline_shaders_[pso]; s.fixed=pipeline_fixed_[pso]; s.raster_known=s.fixed.raster_known; s.depth_known=s.fixed.depth_known; s.blend_known=s.fixed.blend_known; }
void Runtime::set_root_signature(ObjectId id,ObjectId root,bool compute) { std::lock_guard lock(mutex_); touch(id); auto& s=lists_[id].state; if(compute) { if(s.compute_root!=root) s.compute_bindings.clear(); s.compute_root=root; } else { if(s.graphics_root!=root) s.graphics_bindings.clear(); s.graphics_root=root; } }
void Runtime::set_descriptor_heaps(ObjectId id,std::span<const ObjectId> heaps) { std::lock_guard lock(mutex_); touch(id); lists_[id].state.descriptor_heaps={heaps.begin(),heaps.end()}; }
void Runtime::set_root_table(ObjectId id,bool compute,std::uint32_t slot,DescriptorRef base) { std::lock_guard lock(mutex_); touch(id); auto& s=lists_[id].state; (compute?s.compute_bindings:s.graphics_bindings)[slot]={RootBinding::Kind::Table,base}; }
void Runtime::set_root_descriptor(ObjectId id,bool compute,std::uint32_t slot,ObjectId resource,std::uint64_t address,std::optional<BindingKind> kind) { std::lock_guard lock(mutex_); touch(id); RootBinding b; b.kind=RootBinding::Kind::Descriptor; b.resource=resource; b.address=address; b.descriptor_kind=kind; auto& s=lists_[id].state; (compute?s.compute_bindings:s.graphics_bindings)[slot]=std::move(b); }
void Runtime::set_root_constants(ObjectId id,bool compute,std::uint32_t slot,std::uint32_t offset,std::span<const std::uint32_t> values) { std::lock_guard lock(mutex_); touch(id); auto& s=lists_[id].state; auto& b=(compute?s.compute_bindings:s.graphics_bindings)[slot]; if(b.kind!=RootBinding::Kind::Constants) b={}; b.kind=RootBinding::Kind::Constants; constexpr std::size_t limit=64; if(offset>limit || values.size()>limit-offset) { b.constants_overflow=true; incomplete_=true; return; } const auto size=std::max(b.constants.size(),std::size_t(offset)+values.size()); b.constants.resize(size); b.constants_known.resize(size,0); std::copy(values.begin(),values.end(),b.constants.begin()+offset); std::fill(b.constants_known.begin()+offset,b.constants_known.begin()+offset+values.size(),std::uint8_t{1}); }
void Runtime::set_vertex_buffer(ObjectId id,std::uint32_t slot,ObjectId resource,std::uint64_t offset,std::uint64_t bytes) { std::lock_guard lock(mutex_); touch(id); lists_[id].state.vertex_buffers[slot]={resource,offset,bytes}; }
void Runtime::set_index_buffer(ObjectId id,ObjectId resource,std::uint64_t offset,std::uint64_t bytes) { std::lock_guard lock(mutex_); touch(id); lists_[id].state.index_buffer={resource,offset,bytes}; }
void Runtime::set_targets(ObjectId id,std::span<const DescriptorRef> rt,DescriptorRef depth) { std::lock_guard lock(mutex_); touch(id); auto& s=lists_[id].state; s.render_targets={rt.begin(),rt.end()}; s.depth_target=depth; }
void Runtime::set_scissor(ObjectId id,std::int32_t x,std::int32_t y,std::int32_t w,std::int32_t h) { std::lock_guard lock(mutex_); touch(id); lists_[id].state.scissor=Rect{x,y,w,h}; }
void Runtime::set_viewports(ObjectId id,std::span<const Viewport> viewports) { std::lock_guard lock(mutex_); touch(id); if(viewports.size()>16) { incomplete_=true; return; } lists_[id].state.viewports={viewports.begin(),viewports.end()}; }
void Runtime::set_topology(ObjectId id,std::uint32_t topology) { std::lock_guard lock(mutex_); touch(id); auto& s=lists_[id].state; s.topology=topology; s.topology_known=topology!=0; }
void Runtime::set_blend_factor(ObjectId id,std::array<float,4> factor) { std::lock_guard lock(mutex_); touch(id); auto& s=lists_[id].state; s.blend_factor=factor; s.blend_factor_known=true; }
void Runtime::set_stencil_ref(ObjectId id,std::uint32_t reference) { std::lock_guard lock(mutex_); touch(id); auto& s=lists_[id].state; s.stencil_ref=reference; s.stencil_ref_known=true; }
void Runtime::set_shading_rate(ObjectId id,std::uint32_t rate,std::span<const std::uint32_t> combiners) { std::lock_guard lock(mutex_); touch(id); auto& s=lists_[id].state; s.shading_rate=rate; s.shading_rate_combiners={combiners.begin(),combiners.end()}; s.shading_rate_known=combiners.empty()||combiners.size()==2; if(!s.shading_rate_known) incomplete_=true; }
void Runtime::push_accesses(WorkItem& item) {
    const auto& s=item.state;
    const bool graphics=item.kind==WorkKind::Draw||item.kind==WorkKind::DrawIndexed;
    const bool compute=item.kind==WorkKind::Dispatch;
    const bool indirect=item.kind==WorkKind::ExecuteIndirect;
    if(!graphics&&!compute&&!indirect) return;
    const bool shader_work=graphics||compute||indirect;
    if(shader_work && s.shaders.empty()) item.accesses.push_back({{},AccessKind::Unknown,Certainty::Unknown,0,0,"shader identity unavailable"});
    if(indirect) { item.accesses.push_back({{},AccessKind::Unknown,Certainty::Symbolic,0,0,"indirect signature and argument accesses unresolved"}); return; }
    auto descriptor_access=[&](DescriptorRef ref,AccessKind kind,bool table=false) { auto slot=slots_.find({ref.heap,ref.index}); if(!ref.heap || slot==slots_.end() || slot->second.generation!=ref.generation || !slot->second.current.valid) { item.accesses.push_back({{},kind,Certainty::Unknown,0,0,"descriptor generation unavailable"}); return; } item.accesses.push_back({slot->second.current.resource,kind,table?Certainty::Symbolic:Certainty::Known,0,0,table?"descriptor table range/indexing unresolved":""}); };
    auto& bindings=compute?s.compute_bindings:s.graphics_bindings;
    ObjectId root=compute?s.compute_root:s.graphics_root;
    const auto root_it=root_parameters_.find(root);
    bool typed=false;
    for(auto shader:s.shaders) {
        auto sit=shader_bindings_.find(shader);
        if(sit==shader_bindings_.end()) continue;
        typed=true;
        for(const auto& sb:sit->second) {
            if(sb.kind==BindingKind::Sampler) continue;
            bool matched=false;
            if(root_it!=root_parameters_.end()) for(const auto& param:root_it->second) {
                auto bound=bindings.find(param.slot); if(bound==bindings.end()) continue;
                // Root constants occupy a CBV register but carry inline values,
                // not a descriptor or GPU resource dependency. Their captured
                // values remain in PipelineSnapshot::graphics/compute_bindings.
                if(param.kind==RootParameterKind::Constants && bound->second.kind==RootBinding::Kind::Constants && sb.kind==BindingKind::Cbv && param.space==sb.space && param.shader_register>=sb.first_register && (sb.count==UINT32_MAX || std::uint64_t(param.shader_register)<std::uint64_t(sb.first_register)+sb.count)) {
                    matched=true;
                    if(bound->second.constants_overflow) item.accesses.push_back({{},AccessKind::Read,Certainty::Unknown,0,0,"root constants capture exceeded limit"});
                }
                if(param.kind==RootParameterKind::Descriptor && bound->second.kind==RootBinding::Kind::Descriptor && param.descriptor_kind==sb.kind && param.space==sb.space && param.shader_register>=sb.first_register && (sb.count==UINT32_MAX || std::uint64_t(param.shader_register)<std::uint64_t(sb.first_register)+sb.count)) {
                    matched=true; item.accesses.push_back({bound->second.resource,sb.access,bound->second.resource?Certainty::Known:Certainty::Unknown,0,0,"root descriptor"});
                }
                if(param.kind!=RootParameterKind::Table || bound->second.kind!=RootBinding::Kind::Table) continue;
                std::uint64_t append=0;
                for(const auto& range:param.ranges) {
                    std::uint64_t offset=range.table_offset==UINT32_MAX?append:range.table_offset;
                    std::uint64_t count=range.count==UINT32_MAX?UINT64_MAX:range.count;
                    append=count==UINT64_MAX?UINT64_MAX:std::min(UINT64_MAX-offset, count)+offset;
                    if(range.kind!=sb.kind || range.space!=sb.space) continue;
                    std::uint64_t a=std::max(std::uint64_t(sb.first_register),std::uint64_t(range.first_register));
                    std::uint64_t sb_end=sb.count==UINT32_MAX?UINT64_MAX:std::uint64_t(sb.first_register)+sb.count;
                    std::uint64_t range_end=count==UINT64_MAX?UINT64_MAX:std::uint64_t(range.first_register)+count;
                    std::uint64_t end=std::min(sb_end,range_end);
                    if(end<=a) continue;
                    matched=true;
                    auto base=bound->second.table;
                    auto heap_size=descriptor_heap_counts_.find(base.heap);
                    if(!valid(base.heap,ObjectKind::DescriptorHeap)||heap_size==descriptor_heap_counts_.end()||offset==UINT64_MAX||a-range.first_register>UINT64_MAX-offset||std::uint64_t(base.index)>UINT64_MAX-offset-(a-range.first_register)) {
                        item.accesses.push_back({{},sb.access,Certainty::Unknown,0,0,"descriptor table heap/range unavailable"}); continue;
                    }
                    std::uint64_t first=std::uint64_t(base.index)+offset+(a-range.first_register);
                    std::uint64_t length=end==UINT64_MAX?UINT64_MAX:end-a;
                    if(first>=heap_size->second) { item.accesses.push_back({{},sb.access,Certainty::Unknown,0,0,"descriptor range exceeds heap"}); continue; }
                    if(length!=UINT64_MAX && length>std::uint64_t(heap_size->second)-first) { item.accesses.push_back({{},sb.access,Certainty::Unknown,0,0,"declared descriptor range exceeds heap"}); continue; }
                    if(length==UINT64_MAX) length=std::uint64_t(heap_size->second)-first;
                    Access symbolic{{},sb.access,Certainty::Symbolic,0,0,sb.dynamic_indexing?"dynamic descriptor range":"declared descriptor range"};
                    symbolic.descriptor_heap=base.heap; symbolic.descriptor_first=first; symbolic.descriptor_count=length; item.accesses.push_back(std::move(symbolic));
                }
            }
            if(!matched) item.accesses.push_back({{},sb.access,Certainty::Unknown,0,0,"shader binding has no captured root mapping"});
        }
    }
    if(!typed) for(const auto& [_,b]:bindings) if(b.kind==RootBinding::Kind::Table) { Access a{{},AccessKind::Unknown,Certainty::Symbolic,0,0,"descriptor table range/indexing unresolved"}; a.descriptor_heap=b.table.heap; a.descriptor_first=b.table.index; item.accesses.push_back(std::move(a)); } else if(b.kind==RootBinding::Kind::Descriptor) item.accesses.push_back({b.resource,AccessKind::Unknown,b.resource?Certainty::Known:Certainty::Unknown});
    if(graphics) {
        for(const auto& [_,b]:s.vertex_buffers) if(b.resource) item.accesses.push_back({b.resource,AccessKind::Read,Certainty::Known,b.offset,b.bytes});
        if(item.kind==WorkKind::DrawIndexed && s.index_buffer.resource) item.accesses.push_back({s.index_buffer.resource,AccessKind::Read,Certainty::Known,s.index_buffer.offset,s.index_buffer.bytes});
        for(auto ref:s.render_targets) descriptor_access(ref,AccessKind::Write);
        if(s.depth_target.heap) descriptor_access(s.depth_target,AccessKind::ReadWrite);
    }
    for(auto shader:s.shaders) { auto it=shaders_.find(shader); if(it==shaders_.end()) { if(!shader_bindings_.contains(shader)) item.accesses.push_back({{},AccessKind::Unknown,Certainty::Unknown,0,0,"shader declarations unavailable"}); } else item.accesses.insert(item.accesses.end(),it->second.begin(),it->second.end()); }
}
bool Runtime::descriptors_current(const WorkItem& item) const {
    auto current=[&](DescriptorRef ref) { if(!ref.heap) return true; auto it=slots_.find({ref.heap,ref.index}); return it!=slots_.end() && it->second.generation==ref.generation && valid(ref.heap,ObjectKind::DescriptorHeap); };
    for(const auto& [_,binding]:item.state.graphics_bindings) if(binding.kind==RootBinding::Kind::Table && !current(binding.table)) return false;
    for(const auto& [_,binding]:item.state.compute_bindings) if(binding.kind==RootBinding::Kind::Table && !current(binding.table)) return false;
    for(auto ref:item.state.render_targets) if(!current(ref)) return false;
    return current(item.state.depth_target);
}
WorkId Runtime::record_locked(ObjectId id,WorkKind kind,std::span<const Access> access,bool rewrite_eligible,std::string_view coverage) { auto it=lists_.find(id); if(it==lists_.end() || it->second.closed || !valid(id,ObjectKind::CommandList)) { incomplete_=true; return {}; } WorkId wid{next_work_++}; ++it->second.sequence; it->second.last_was_clear=false; WorkItem item; item.id=wid; item.list=id; item.list_generation=it->second.generation; item.kind=kind; item.rewrite_eligible=rewrite_eligible; item.state=it->second.state; item.accesses.assign(access.begin(),access.end()); for(auto& a:item.accesses) if(a.certainty==Certainty::Known) { if(!valid(a.resource,ObjectKind::Resource)) { a.certainty=Certainty::Unknown; incomplete_=true; } else if(a.bytes&&resources_.contains(a.resource)) { const auto size=resources_.at(a.resource).bytes; if(a.offset>size||a.bytes>size-a.offset) { a.certainty=Certainty::Unknown; incomplete_=true; } } } if(!it->second.work.empty()) item.dependencies.push_back(it->second.work.back()); if(kind==WorkKind::Unknown) { item.supported=false; item.coverage=coverage.empty()?"unknown work":std::string(coverage); incomplete_=true; } push_accesses(item); if(max_work_==0 || it->second.recorded.size()>=max_work_) { ++dropped_; incomplete_=true; it->second.poisoned=true; return wid; } auto retained=std::make_shared<const WorkItem>(std::move(item)); it->second.work.push_back(wid); it->second.recorded.push_back(retained); if(work_.size()>=max_work_) { work_.pop_front(); ++dropped_; history_truncated_=true; } work_.push_back(std::move(retained)); return wid; }
WorkId Runtime::record_work(ObjectId id,WorkKind kind,std::span<const Access> access) { std::lock_guard lock(mutex_); return record_locked(id,kind,access); }
bool Runtime::record_clear(ObjectId id,DescriptorRef target,std::span<const std::uint32_t> signature) { std::lock_guard lock(mutex_); auto it=lists_.find(id); if(it==lists_.end()) { incomplete_=true; return false; } auto slot=slots_.find({target.heap,target.index}); bool format=signature.size()>=5 && signature[4]<=(signature.size()-5)/4 && signature.size()==5+4*std::size_t(signature[4]); bool exact=format&&valid(target.heap,ObjectKind::DescriptorHeap)&&slot!=slots_.end()&&slot->second.generation==target.generation&&slot->second.current.valid&&slot->second.current.kind==ViewKind::Rtv&&valid(slot->second.current.resource,ObjectKind::Resource); bool redundant=exact&&!it->second.poisoned&&max_work_>0&&it->second.recorded.size()<max_work_&&it->second.last_was_clear&&it->second.last_clear_target==target&&std::equal(signature.begin(),signature.end(),it->second.last_clear_signature.begin(),it->second.last_clear_signature.end()); Access access{exact?slot->second.current.resource:ObjectId{},AccessKind::Write,exact?Certainty::Known:Certainty::Unknown}; auto wid=record_locked(id,WorkKind::Clear,std::span<const Access>(&access,1),redundant); if(!wid) return false; auto& list=lists_[id]; redundant=redundant&&!list.poisoned&&!list.recorded.empty()&&list.recorded.back()->id==wid; list.last_was_clear=exact; list.last_clear_target=target; list.last_clear_signature.assign(signature.begin(),signature.end()); return redundant; }
void Runtime::barrier(ObjectId id,ObjectId resource,std::uint64_t before,std::uint64_t after,bool known) { std::lock_guard lock(mutex_); auto work=record_locked(id,WorkKind::Barrier,{}); if(work) { if(max_history_&&transitions_.size()>=max_history_) { transitions_.pop_front(); ++dropped_; history_truncated_=true; } transitions_.push_back({work,resource,before,after,known,false}); } if(!known) incomplete_=true; }
void Runtime::alias_barrier(ObjectId id,ObjectId before,ObjectId after) { std::lock_guard lock(mutex_); auto work=record_locked(id,WorkKind::Barrier,{}); if(work) { if(max_history_&&transitions_.size()>=max_history_) { transitions_.pop_front(); ++dropped_; } transitions_.push_back({work,after,before.value,after.value,false,true}); } incomplete_=true; }
void Runtime::execute_bundle(ObjectId id,ObjectId bundle) { std::lock_guard lock(mutex_); Access a{bundle,AccessKind::Unknown,Certainty::Symbolic,0,0,"bundle execution"}; record_locked(id,WorkKind::Bundle,std::span<const Access>(&a,1)); }
void Runtime::query(ObjectId id,ObjectId query_heap,ObjectId destination) { std::lock_guard lock(mutex_); if(!valid(query_heap,ObjectKind::QueryHeap)) { incomplete_=true; return; } if(destination) { Access a{destination,AccessKind::Write,valid(destination,ObjectKind::Resource)?Certainty::Known:Certainty::Unknown}; record_locked(id,WorkKind::Query,std::span<const Access>(&a,1)); } else record_locked(id,WorkKind::Query,{}); }
SubmissionId Runtime::submit(ObjectId queue,std::span<const ObjectId> lists) { std::lock_guard lock(mutex_); if(!valid(queue,ObjectKind::Queue)) { incomplete_=true; return {}; } Submission s; s.id={next_submission_++}; s.queue=queue; s.complete=true; if(queue_last_[queue].value) s.dependencies.push_back(queue_last_[queue]); for(auto dep:queue_waits_[queue]) if(dep.value) s.dependencies.push_back(dep); queue_waits_[queue].clear(); for(auto& pending:pending_waits_) if(pending.queue==queue) { s.complete=false; if(!pending.first_after) pending.first_after=s.id; } for(auto id:lists) { auto it=lists_.find(id); if(it==lists_.end()||!it->second.closed) { s.complete=false; incomplete_=true; continue; } s.work.insert(s.work.end(),it->second.work.begin(),it->second.work.end()); for(const auto& w:it->second.recorded) if(!descriptors_current(*w)) s.complete=false; } if(!s.complete) ++uncertain_submissions_; queue_last_[queue]=s.id; if(max_history_ && submissions_.size()>=max_history_) { submissions_.pop_front(); ++dropped_; history_truncated_=true; } submissions_.push_back(s); return s.id; }
void Runtime::signal(ObjectId queue,ObjectId fence,std::uint64_t value) { std::lock_guard lock(mutex_); auto submission=queue_last_[queue]; fence_signals_[fence][value]=submission; if(submission) for(auto it=pending_waits_.begin();it!=pending_waits_.end();) { if(it->fence!=fence||value<it->value) { ++it; continue; } for(auto& edge:fences_) if(!edge.signal&&!edge.satisfied&&edge.queue==it->queue&&edge.fence==fence&&edge.value==it->value) { edge.submission=submission; edge.satisfied=true; } if(it->first_after) for(auto& s:submissions_) if(s.queue==it->queue&&s.id.value>=it->first_after.value) s.dependencies.push_back(submission); if(!it->first_after) queue_waits_[it->queue].push_back(submission); it=pending_waits_.erase(it); } if(max_history_&&fences_.size()>=max_history_) { fences_.pop_front(); ++dropped_; history_truncated_=true; } fences_.push_back({queue,fence,value,submission,true,bool(submission)}); }
void Runtime::wait(ObjectId queue,ObjectId fence,std::uint64_t value) { std::lock_guard lock(mutex_); SubmissionId dependency{}; auto it=fence_signals_.find(fence); if(it!=fence_signals_.end()) { auto p=it->second.lower_bound(value); if(p!=it->second.end()) dependency=p->second; } if(dependency) queue_waits_[queue].push_back(dependency); else if(max_history_&&pending_waits_.size()>=max_history_) { ++dropped_; incomplete_=true; } else pending_waits_.push_back({queue,fence,value,{}}); if(max_history_&&fences_.size()>=max_history_) { fences_.pop_front(); ++dropped_; history_truncated_=true; } fences_.push_back({queue,fence,value,dependency,false,bool(dependency)}); }
void Runtime::present(ObjectId swapchain,ObjectId queue) { present(swapchain,queue,{},0,false); }
void Runtime::present(ObjectId swapchain,ObjectId queue,ObjectId backbuffer,std::int32_t result,bool test_only) { std::lock_guard lock(mutex_); if(result!=0 || test_only) return; if(max_history_&&presents_.size()>=max_history_) { presents_.pop_front(); ++dropped_; history_truncated_=true; } presents_.push_back({swapchain,queue,backbuffer,next_present_++,swapchain_generations_[swapchain],queue_last_[queue],result}); }
void Runtime::resize_swapchain(ObjectId swapchain) { std::lock_guard lock(mutex_); ++swapchain_generations_[swapchain]; }
void Runtime::unsupported(ObjectId id,std::string_view api) { std::lock_guard lock(mutex_); if(!id.value) { if(interface_coverage_.size()<256 || interface_coverage_.contains(std::string(api))) ++interface_coverage_[std::string(api)]; incomplete_=true; return; } record_locked(id,WorkKind::Unknown,{},false,api); lists_[id].poisoned=true; incomplete_=true; }
void Runtime::note_coverage(std::string_view api) { std::lock_guard lock(mutex_); if(interface_coverage_.size()<256 || interface_coverage_.contains(std::string(api))) ++interface_coverage_[std::string(api)]; }
void Runtime::touch_command(ObjectId id) { std::lock_guard lock(mutex_); touch(id); }
std::shared_ptr<const WorkItem> Runtime::last_work(ObjectId id) const { std::lock_guard lock(mutex_); auto it=lists_.find(id); if(it==lists_.end()||it->second.recorded.empty()) return {}; return it->second.recorded.back(); }
std::uint64_t Runtime::state_sequence(ObjectId id) const { std::lock_guard lock(mutex_); auto it=lists_.find(id); return it==lists_.end()?0:it->second.sequence; }
IrSnapshot Runtime::snapshot() const { std::lock_guard lock(mutex_); IrSnapshot s; for(auto& [_,o]:objects_) s.objects.push_back(o); for(auto& [id,r]:resources_) s.resources.push_back({id,r.heap,r.bytes,r.offset,r.shape}); for(auto& [id,declared]:shaders_) s.shaders.push_back({id,declared,shader_bindings_.contains(id)?shader_bindings_.at(id):std::vector<ShaderBinding>{}}); for(auto& [id,bindings]:shader_bindings_) if(!shaders_.contains(id)) s.shaders.push_back({id,{},bindings}); for(auto& [id,params]:root_parameters_) s.root_signatures.push_back({id,params}); std::sort(s.shaders.begin(),s.shaders.end(),[](auto& a,auto& b){return a.shader<b.shader;}); for(auto& [_,slot]:slots_) s.descriptors.push_back(slot.current); std::sort(s.descriptors.begin(),s.descriptors.end(),[](auto& a,auto& b){ return a.ref.heap==b.ref.heap?a.ref.index<b.ref.index:a.ref.heap<b.ref.heap; }); s.work.assign(work_.begin(),work_.end()); s.submissions.assign(submissions_.begin(),submissions_.end()); s.fences.assign(fences_.begin(),fences_.end()); s.transitions.assign(transitions_.begin(),transitions_.end()); s.presents.assign(presents_.begin(),presents_.end()); s.interface_coverage.assign(interface_coverage_.begin(),interface_coverage_.end()); s.total_work=next_work_-1; s.dropped=dropped_; s.uncertain_submissions=uncertain_submissions_; s.incomplete=incomplete_||!pending_waits_.empty(); s.history_truncated=history_truncated_; return s; }
Runtime& runtime() { static Runtime instance; return instance; }
} // namespace arc::arc2

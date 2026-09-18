#include "arc/gpu_attribution.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace arc {
std::optional<double> raster_coverage_upper(const RasterRegion& r) noexcept {
    const auto valid=[](const ScreenRect& a) {
        return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.width)&&std::isfinite(a.height)
            && a.width>=0 && a.height>=0 && std::isfinite(a.x+a.width)&&std::isfinite(a.y+a.height);
    };
    if(!r.known || !r.width || !r.height || !valid(r.viewport) || !valid(r.scissor)) return {};
    const double left=std::max({0.0,r.viewport.x,r.scissor.x});
    const double top=std::max({0.0,r.viewport.y,r.scissor.y});
    const double right=std::min({double(r.width),r.viewport.x+r.viewport.width,r.scissor.x+r.scissor.width});
    const double bottom=std::min({double(r.height),r.viewport.y+r.viewport.height,r.scissor.y+r.scissor.height});
    return std::clamp(std::max(0.0,right-left)*std::max(0.0,bottom-top)/(double(r.width)*r.height),0.0,1.0);
}
std::optional<double> gpu_duration_ms(const GpuTimestampSample& s) noexcept {
    if(!s.completed || !s.frequency || s.end<s.begin) return {};
    const double ms=double(s.end-s.begin)*1000.0/double(s.frequency);
    return std::isfinite(ms) ? std::optional<double>{ms} : std::nullopt;
}
void GpuAttributionGraph::clear() {
    commands_.clear();clocks_.clear();writers_.clear();signals_.clear();nodes_.clear();edges_.clear();errors_=0;
}
bool GpuAttributionGraph::begin(CommandId id) {
    if(!id || (!commands_.contains(id) && commands_.size()>=limits_.commands)) return fail();
    commands_[id]={}; return true;
}
bool GpuAttributionGraph::record(CommandId id,const WorkObservation& w) {
    auto it=commands_.find(id);
    if(it==commands_.end() || it->second.closed || it->second.works.size()>=limits_.works_per_command ||
       w.accesses.size()>limits_.accesses_per_work) return fail();
    if(std::any_of(w.accesses.begin(),w.accesses.end(),[](const auto& a){return !a.resource;})) return fail();
    it->second.works.push_back(w);return true;
}
bool GpuAttributionGraph::close(CommandId id) {
    auto it=commands_.find(id);if(it==commands_.end()||it->second.closed)return fail();
    it->second.closed=true;return true;
}
void GpuAttributionGraph::retire_command(CommandId id) {commands_.erase(id);}
GpuAttributionGraph::Clock* GpuAttributionGraph::queue(QueueId id) {
    if(!id || (!clocks_.contains(id)&&clocks_.size()>=limits_.queues)){fail();return nullptr;}
    return &clocks_[id];
}
WorkId GpuAttributionGraph::execute(QueueId q,CommandId cmd,const WorkObservation& w) {
    auto* clock=queue(q);if(!clock)return 0;
    if(nodes_.size()>=limits_.nodes){fail();return 0;}
    ++(*clock)[q];
    const auto ordered=[&](const Clock& earlier){
        for(const auto& [id,value]:earlier){const auto it=clock->find(id);if(it==clock->end()||it->second<value)return false;}
        return true;
    };
    const WorkId id=nodes_.size()+1;
    AttributionNode node{id,q,cmd,w,{}, {},!w.bindings_complete};
    if(w.kind==GpuWorkKind::Draw)node.local_coverage_upper=raster_coverage_upper(w.raster);
    // Read against all outstanding possible versions before adding this work's writes.
    for(const auto& a:w.accesses)if(!a.write){
        auto it=writers_.find(a.resource);
        if(it==writers_.end()||it->second.empty()){node.unresolved_inputs=true;continue;}
        for(const auto& writer:it->second){
            if(edges_.size()>=limits_.edges){fail();node.unresolved_inputs=true;break;}
            const bool sync=ordered(writer.clock);
            const bool observed=a.evidence==AccessEvidence::Observed&&writer.evidence==AccessEvidence::Observed;
            edges_.push_back({writer.id,id,a.resource,sync,observed});
            if(!sync||!observed)node.unresolved_inputs=true;
        }
    }
    for(const auto& a:w.accesses)if(a.write){
        if(!writers_.contains(a.resource)&&writers_.size()>=limits_.resources){fail();continue;}
        auto& prior=writers_[a.resource];
        // A partial write can preserve earlier pixels. Unsynchronized writes never erase evidence.
        if(a.full_overwrite&&a.evidence==AccessEvidence::Observed)
            std::erase_if(prior,[&](const auto& writer){return ordered(writer.clock);});
        if(prior.size()>=limits_.nodes){fail();continue;}
        prior.push_back({id,*clock,a.evidence});
    }
    nodes_.push_back(std::move(node));return id;
}
std::vector<WorkId> GpuAttributionGraph::submit(QueueId q,CommandId cmd) {
    auto it=commands_.find(cmd);
    if(it==commands_.end()||!it->second.closed){fail();return {};}
    std::vector<WorkId> ids;
    for(const auto& w:it->second.works){auto id=execute(q,cmd,w);if(!id)break;ids.push_back(id);}
    return ids;
}
bool GpuAttributionGraph::signal(QueueId q,std::uint64_t fence,std::uint64_t value){
    auto* clock=queue(q);if(!clock)return false;
    if(!fence||signals_.size()>=limits_.signals)return fail();
    // Capture contract requires monotonic, single-owner fences; ambiguous reuse fails closed.
    for(const auto& s:signals_)if(s.fence==fence&&(s.value>=value||s.owner!=q))return fail();
    ++(*clock)[q]; signals_.push_back({fence,value,q,*clock});return true;
}
bool GpuAttributionGraph::wait(QueueId q,std::uint64_t fence,std::uint64_t value){
    auto* clock=queue(q);if(!clock)return false;
    const Signal* selected=nullptr;
    for(const auto& s:signals_)if(s.fence==fence&&s.value>=value&&(!selected||s.value<selected->value))selected=&s;
    if(!selected)return fail(); // Forward waits need replay; never infer order from CPU arrival.
    for(const auto& [id,v]:selected->clock)(*clock)[id]=std::max((*clock)[id],v);
    ++(*clock)[q];return true;
}
WorkId GpuAttributionGraph::present(QueueId q,ResourceId resource){
    if(!resource){fail();return 0;}
    WorkObservation w;w.kind=GpuWorkKind::Present;w.bindings_complete=true;
    w.accesses.push_back({resource,false,AccessEvidence::Observed,false});return execute(q,0,w);
}
bool GpuAttributionGraph::timing(WorkId id,const GpuTimestampSample& s){
    if(!id||id>nodes_.size()||nodes_[id-1].gpu_ms)return fail();
    const auto ms=gpu_duration_ms(s);if(!ms)return fail();nodes_[id-1].gpu_ms=ms;return true;
}
AttributionSlice GpuAttributionGraph::ancestors(WorkId id) const {
    AttributionSlice result;result.complete=complete();
    if(!id||id>nodes_.size()){result.complete=false;return result;}
    std::vector<std::vector<const AttributionEdge*>> incoming(nodes_.size()+1);
    for(const auto& edge:edges_)incoming[edge.consumer].push_back(&edge);
    std::set<WorkId> seen{id};std::vector<WorkId> pending{id};
    while(!pending.empty()){
        auto n=pending.back();pending.pop_back();if(nodes_[n-1].unresolved_inputs)result.complete=false;
        for(const auto* edge:incoming[n]){const auto& e=*edge;
            if(!e.synchronized||!e.observed)result.complete=false;
            if(seen.insert(e.producer).second)pending.push_back(e.producer);
        }
    }
    seen.erase(id);result.contributors.assign(seen.begin(),seen.end());return result;
}
void GpuAttributionGraph::write_json(std::ostream& out) const {
    out<<"{\"schema\":1,\"observer_only\":true,\"capture_complete\":"<<(complete()?"true":"false")
       <<",\"errors\":"<<errors_<<",\"nodes\":[";
    bool first=true;
    for(const auto& n:nodes_){
        if(!first)out<<',';first=false;
        out<<"{\"id\":"<<n.id<<",\"queue\":"<<n.queue<<",\"command\":"<<n.command
           <<",\"kind\":"<<int(n.work.kind)<<",\"items\":"<<n.work.items<<",\"copy_bytes\":"<<n.work.copy_bytes
           <<",\"pipeline\":"<<n.work.pipeline<<",\"unresolved_inputs\":"<<(n.unresolved_inputs?"true":"false")
           <<",\"local_raster_coverage_upper\":";
        if(n.local_coverage_upper)out<<*n.local_coverage_upper;else out<<"null";
        out<<",\"gpu_ms\":";if(n.gpu_ms)out<<*n.gpu_ms;else out<<"null";
        const auto& r=n.work.raster;
        out<<",\"raster\":{\"known\":"<<(r.known?"true":"false")<<",\"width\":"<<r.width<<",\"height\":"<<r.height;
        const auto rect=[&](const char* key,const ScreenRect& value){
            out<<",\""<<key<<"\":[";bool first_value=true;
            for(double component:{value.x,value.y,value.width,value.height}){
                if(!first_value)out<<',';first_value=false;
                if(std::isfinite(component))out<<component;else out<<"null";
            }
            out<<']';
        };
        rect("viewport",r.viewport);rect("scissor",r.scissor);out<<'}';
        out<<",\"accesses\":[";bool fa=true;
        for(const auto& a:n.work.accesses){if(!fa)out<<',';fa=false;out<<"{\"resource\":"<<a.resource<<",\"write\":"<<(a.write?"true":"false")<<",\"observed\":"<<(a.evidence==AccessEvidence::Observed?"true":"false")<<",\"full_overwrite\":"<<(a.full_overwrite?"true":"false")<<'}';}
        out<<"]}";
    }
    out<<"],\"edges\":[";first=true;
    for(const auto& e:edges_){if(!first)out<<',';first=false;out<<"{\"producer\":"<<e.producer<<",\"consumer\":"<<e.consumer<<",\"resource\":"<<e.resource<<",\"synchronized\":"<<(e.synchronized?"true":"false")<<",\"observed\":"<<(e.observed?"true":"false")<<'}';}
    out<<"]}";
}
} // namespace arc

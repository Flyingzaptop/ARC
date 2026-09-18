#include "arc/gpu_attribution.hpp"
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
using namespace arc;
void check(bool ok,const char* why){if(!ok){std::cerr<<why<<'\n';std::exit(1);}}
WorkObservation work(ResourceId input,ResourceId output,bool full=true){
    WorkObservation w;w.bindings_complete=true;
    if(input)w.accesses.push_back({input,false,AccessEvidence::Observed,false});
    if(output)w.accesses.push_back({output,true,AccessEvidence::Observed,full});
    return w;
}
WorkId run(GpuAttributionGraph& g,QueueId q,CommandId c,const WorkObservation& w){
    check(g.begin(c)&&g.record(c,w)&&g.close(c),"record");
    const auto ids=g.submit(q,c);check(ids.size()==1,"submit");return ids[0];
}
int main(){
    GpuAttributionGraph g;
    // Producer chain and dead branch: topology, not labels, determines ancestors.
    auto a=run(g,1,1,work(0,10));auto dead=run(g,1,2,work(0,99));
    auto b=run(g,1,3,work(10,20));auto c=run(g,1,4,work(20,30));auto p=g.present(1,30);
    check(g.ancestors(p).contributors==std::vector<WorkId>({a,b,c}),"known dependency chain");
    check(g.ancestors(p).complete && dead==2,"dead branch excluded");
    // Submission order wins over recording order; recording alone has no execution effect.
    g.clear();check(g.begin(1)&&g.record(1,work(0,10))&&g.close(1),"record first");
    run(g,1,2,work(10,20));g.submit(1,1);
    check(g.nodes()[0].unresolved_inputs&&g.edges().empty(),"no backward invented causality");
    // Queue ordering is not inferred from CPU observation order.
    g.clear();run(g,1,1,work(0,10));auto reader=run(g,2,2,work(10,20));
    check(!g.edges()[0].synchronized&&!g.ancestors(reader).complete,"unsynchronized queues");
    g.clear();a=run(g,1,1,work(0,10));check(g.signal(1,42,3)&&g.wait(2,42,3),"fence chain");
    b=run(g,2,2,work(10,20));check(g.edges()[0].synchronized&&g.ancestors(b).complete,"synchronized dependency");
    check(!g.wait(2,43,1)&&!g.complete(),"missing fence fails closed");
    g.clear();check(g.signal(1,42,3)&&!g.signal(2,42,4),"multiple fence owners rejected");
    // Partial writes preserve earlier output. Full writes kill only synchronized versions.
    g.clear();a=run(g,1,1,work(0,10));b=run(g,1,2,work(0,10,false));p=g.present(1,10);
    check(g.ancestors(p).contributors==std::vector<WorkId>({a,b}),"partial overwrite");
    c=run(g,1,3,work(0,10));p=g.present(1,10);
    check(g.ancestors(p).contributors==std::vector<WorkId>({c}),"full overwrite");
    g.clear();a=run(g,1,1,work(0,10));b=run(g,2,2,work(0,10));p=g.present(2,10);
    check(g.ancestors(p).contributors.size()==2&&!g.ancestors(p).complete,"racing overwrite retained");
    // Re-execution and reset create distinct work, without altering submitted nodes.
    g.clear();a=run(g,1,1,work(0,10));auto replay=g.submit(1,1);check(replay[0]!=a,"replay identity");
    check(g.begin(1),"reset");g.retire_command(1);check(g.nodes().size()==2&&g.command_count()==0,"retirement");
    g.clear();auto uncertain=work(10,20);uncertain.accesses[0].evidence=AccessEvidence::Possible;
    run(g,1,1,work(0,10));b=run(g,1,2,uncertain);check(!g.edges()[0].observed&&!g.ancestors(b).complete,"possible != observed");
    // Read/write same resource reads the previous version, never creates self edges.
    g.clear();a=run(g,1,1,work(0,10));b=run(g,1,2,work(10,10));check(g.edges()[0].producer==a&&g.edges()[0].consumer==b,"read modify write");
    RasterRegion r{100,100,{0,0,100,100},{0,0,25,40},true};
    check(std::abs(*raster_coverage_upper(r)-.1)<1e-9,"clipped coverage");
    r.scissor={0.1,0.1,0.1,0.1};check(std::abs(*raster_coverage_upper(r)-.0001)<1e-12,"subpixel coverage rounds outwards");
    r.scissor={200,0,1,1};check(*raster_coverage_upper(r)==0,"empty intersection");
    r.scissor={-10,-10,200,200};check(*raster_coverage_upper(r)==1,"target clamp");
    r.viewport.width=std::numeric_limits<double>::quiet_NaN();check(!raster_coverage_upper(r),"NaN unknown");
    check(!gpu_duration_ms({1,2,0,true})&&!gpu_duration_ms({2,1,1000,true})&&!gpu_duration_ms({1,2,1000,false}),"invalid GPU query");
    check(*gpu_duration_ms({100,600,1000000,true})==.5,"timestamp units");
    check(g.timing(a,{100,600,1000000,true})&&!g.timing(a,{100,600,1000000,true}),"timing ownership");
    AttributionLimits limits;limits.nodes=2;limits.commands=2;limits.works_per_command=2;limits.edges=1;
    GpuAttributionGraph bounded(limits);run(bounded,1,1,work(0,10));run(bounded,1,2,work(10,20));
    check(bounded.present(1,20)==0&&!bounded.complete()&&bounded.nodes().size()==2,"bounded nodes");
    bounded.clear();for(int i=0;i<10000;++i){check(bounded.begin(1),"churn begin");bounded.retire_command(1);}
    check(bounded.command_count()==0&&bounded.complete(),"bounded lifetime churn");
    check(bounded.begin(1)&&bounded.begin(2)&&!bounded.begin(3)&&bounded.command_count()==2,"command capacity");
    bounded.clear();check(bounded.begin(1)&&bounded.record(1,work(0,1))&&bounded.record(1,work(0,2))&&!bounded.record(1,work(0,3)),"record capacity");
    check(bounded.begin(2)&&!bounded.record(2,work(0,3)),"aggregate pending budget");
    bounded.retire_command(1);check(bounded.record(2,work(0,3)),"retirement releases pending budget");
    limits.nodes=16;limits.resources=1;GpuAttributionGraph resource_bound(limits);
    run(resource_bound,1,1,work(0,1));run(resource_bound,1,2,work(0,2));check(!resource_bound.complete(),"resource capacity");
    limits.resources=8;limits.edges=1;GpuAttributionGraph edge_bound(limits);
    run(edge_bound,1,1,work(0,1));run(edge_bound,1,1,work(0,1,false));p=edge_bound.present(1,1);
    check(edge_bound.edges().size()==1&&!edge_bound.ancestors(p).complete,"edge overflow cannot yield complete slice");
    limits.queues=1;GpuAttributionGraph queue_bound(limits);run(queue_bound,1,1,work(0,1));
    check(queue_bound.present(2,1)==0&&!queue_bound.complete(),"queue capacity");
    GpuAttributionGraph invalid;check(!invalid.record(7,work(0,1))&&!invalid.close(7)&&invalid.submit(1,7).empty(),"lifecycle misuse");
    // Local coverage does not propagate through arbitrary composition, and compute
    // has no raster coverage even if a stale viewport exists in host state.
    g.clear();auto raster=work(0,1);raster.raster={100,100,{0,0,100,100},{0,0,10,10},true};
    a=run(g,1,1,raster);auto compute=work(1,2);compute.kind=GpuWorkKind::Dispatch;compute.raster=raster.raster;
    b=run(g,1,2,compute);check(g.nodes()[a-1].local_coverage_upper&& !g.nodes()[b-1].local_coverage_upper,"coverage provenance");
    check(g.timing(a,{100,600,1000000,true}),"measured timing");
    std::ostringstream json;g.write_json(json);check(json.str().find("\"gpu_ms\":0.5")!=std::string::npos,"cost export");
    std::cout<<"Mega D attribution: dependency, sync, overwrite, uncertainty, region, timing and bounds PASS\n";
}

"""Compare retained evidence only; legacy and ARC2 completeness flags differ."""
import argparse,json,pathlib

def report(legacy,new):
 a=json.loads(legacy.read_text());b=json.loads(new.read_text())
 nodes=a['graph']['nodes'];work=b['work'];access=[x for w in work for x in w.get('access',[])]
 return {
  'scope':'owned compute binding fixture; graph node types differ and extra ARC2 barriers/queries are not extra GPU work',
  'legacy':{'graph_nodes':len(nodes),'unresolved_nodes':sum(n.get('unresolved_inputs',False) for n in nodes),'access_edges':sum(len(n.get('accesses',[])) for n in nodes),'graph_errors':a['graph']['errors'],'shader_access_complete':a['shader_access_complete'],'capture_state_complete':a['capture_state_complete'],'present_queue_known':a['present_queue_known']},
  'arc2':{'recorded_work':b['total_work'],'retained_work':len(work),'known_accesses':sum(x[2]==0 for x in access),'symbolic_accesses':sum(x[2]==1 for x in access),'unknown_accesses':sum(x[2]==2 for x in access),'unknown_work':sum(w.get('supported') is False for w in work),'declared_shaders':len(b.get('shaders',[])),'root_signatures':len(b.get('root_signatures',[])),'incomplete':b['incomplete'],'history_truncated':b.get('history_truncated'),'uncertain_submissions':b.get('uncertain_submissions'),'dispatch_constants':[w['state']['compute_bindings'] for w in work if w['kind']==2],'heap_intervals':[x[6:9] for x in access if len(x)>=9 and x[6]]},
  'limits':['Known means recorded identity, not proof of all shader execution effects.','Symbolic declared bindings are not exact runtime indices.','Legacy unresolved-node and ARC2 access certainty are different units; no percentage-reduction claim.','This control has no Present; use native-lab frame control for Present links.']}
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('legacy',type=pathlib.Path);p.add_argument('arc2',type=pathlib.Path);p.add_argument('out',type=pathlib.Path);a=p.parse_args();a.out.write_text(json.dumps(report(a.legacy,a.arc2),indent=2))

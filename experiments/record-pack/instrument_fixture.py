"""Temporary, explicitly source-assisted instrumentation of the selected fixture."""
import argparse,difflib,hashlib,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def main():
    p=argparse.ArgumentParser();p.add_argument('renderer_source',type=Path);p.add_argument('checkpoint',type=Path);a=p.parse_args();a.checkpoint.mkdir(parents=True,exist_ok=True)
    raw=a.renderer_source.read_bytes();backup=a.checkpoint/'wiRenderer.before.bin'
    if backup.exists() and backup.read_bytes()!=raw:raise ValueError('Source differs from saved pre-instrumentation checkpoint')
    exe=a.renderer_source.parent.parent/'BUILD/x64/Release/Tests/Tests.exe'
    exe_backup=a.checkpoint/'Tests.original.exe'
    if not exe.exists():raise ValueError('Build a baseline fixture before instrumentation')
    if exe_backup.exists() and hashlib.sha256(exe_backup.read_bytes()).digest()!=hashlib.sha256(exe.read_bytes()).digest():raise ValueError('Use a fresh checkpoint for the changed baseline EXE')
    if not exe_backup.exists():shutil.copy2(exe,exe_backup)
    backup.write_bytes(raw);s=raw.decode('utf-8-sig').replace('\r\n','\n');before=s
    edits=[('#include "ArcResidentProbe.h"','#include "ArcResidentProbe.h"\n#include "'+(ROOT/'experiments/record-pack/pack_probe.hpp').as_posix()+'"'),
        ('arc_resident_probe::Scope arc_mesh_scope(arc_resident_probe::consume);','arc_pack_probe::Scope arc_pack_scope(device->GetFrameCount(),uint32_t(renderQueue.size()),uint32_t(renderPass),vis.scene->weather.IsOceanEnabled());\n\tarc_resident_probe::Scope arc_mesh_scope(arc_resident_probe::consume);'),
        ('auto batch_flush = [&]()\n\t{','auto batch_flush = [&]()\n\t{\n        arc_pack_probe::Flush arc_flush_measure(arc_pack_scope,instancedBatch.instanceCount!=0);'),
        ('uint32_t instanceCount = 0;\n\tfor (const RenderBatch& batch : renderQueue.batches)','uint32_t instanceCount = 0;\n    arc_pack_scope.row.forward=forwardLightmaskRequest;arc_pack_scope.row.stencil=stencil_usage;arc_pack_scope.begin();\n\tfor (const RenderBatch& batch : renderQueue.batches)'),
        ('\n\tbatch_flush();\n\n\tdevice->EventEnd(cmd);\n}\n\nvoid RenderImpostors(','\n    arc_pack_scope.end(instanceCount);\n\tbatch_flush();\n    arc_pack_probe::pack(arc_pack_scope,renderQueue,*vis.scene,instances.data);\n\n\tdevice->EventEnd(cmd);\n}\n\nvoid RenderImpostors(')]
    for old,new in edits:
        if s.count(old)!=1:raise ValueError('Instrumentation site not unique')
        s=s.replace(old,new,1)
    (a.checkpoint/'patch.diff').write_text(''.join(difflib.unified_diff(before.splitlines(True),s.splitlines(True),fromfile='wiRenderer.cpp',tofile='wiRenderer.cpp')),encoding='utf-8')
    a.renderer_source.write_text(s,encoding='utf-8')
    print('Original source SHA256',hashlib.sha256(raw).hexdigest())
if __name__=='__main__':main()

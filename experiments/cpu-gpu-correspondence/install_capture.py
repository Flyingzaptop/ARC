"""Fixture capture sites are manual; the correspondence learner receives raw bytes only."""
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
W=Path('C:/Users/r3d_flzp/ARC-Hardening-GPU/WickedEngine')
backup=ROOT/'build/correspondence-backup';backup.mkdir(exist_ok=True)
header=(ROOT/'experiments/cpu-gpu-correspondence/gpu_audit.hpp').as_posix()
p=W/'WickedEngine/wiRenderer.cpp';s=p.read_text();anchor='PushBarrier(GPUBarrier::Buffer(&vis.scene->instanceBuffer, ResourceState::COPY_DST, ResourceState::SHADER_RESOURCE));';assert s.count(anchor)==1
s='#include "'+header+'"\n'+s;s=s.replace(anchor,'arc_pack_gpu_audit::enqueue(*vis.scene,cmd);\n\t\t'+anchor);p.write_text(s)
p=W/'Samples/Tests/ArcWickedBridge.cpp';b=p.read_bytes();saved=backup/'ArcWickedBridge.cpp'
if saved.exists() and saved.read_bytes()!=b:raise RuntimeError('restore bridge first')
saved.write_bytes(b);s=b.decode('utf-8-sig').replace('\r\n','\n');assert s.count('arc_resident_probe::report();')==1
s=s.replace('#include "ArcResidentProbe.h"','#include "ArcResidentProbe.h"\n#include "'+header+'"').replace('arc_resident_probe::report();','arc_pack_gpu_audit::poll();\n            arc_resident_probe::report();');p.write_text(s)

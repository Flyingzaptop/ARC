"""Add only generic DLL loading/profiling to the existing owned Wicked fixture.

The previous host-action bridge must run with ARC_WICKED_EXPERIMENT_MODE=off.
This script does not alter the rendering backend or provide optimizer semantics.
"""
import argparse
from pathlib import Path
import shutil

parser=argparse.ArgumentParser()
parser.add_argument("wicked",type=Path)
args=parser.parse_args()
directory=args.wicked.resolve()/"Samples/Tests"
source=directory/"main_Windows.cpp"
text=source.read_text()
if '#include "arc_generic_probe.h"' not in text:
    changes={
        '#include "ArcWickedBridge.h"':'#include "ArcWickedBridge.h"\n#include "arc_generic_probe.h"',
        '    // TODO: Place code here.':'    arc_generic_benchmark::initialize();\n    // TODO: Place code here.',
        'if (!arc_wicked::Finished()) tests.Run();':'if (!arc_wicked::Finished()) { tests.Run(); arc_generic_benchmark::tick(); }',
        'return (int) msg.wParam;':'arc_generic_benchmark::finish();\n\treturn (int) msg.wParam;',
    }
    if any(text.count(old)!=1 for old in changes):
        raise RuntimeError("Wicked harness shape changed; inspect before patching")
    backup=source.with_suffix('.cpp.arc-generic-original')
    if backup.exists():
        raise RuntimeError("Existing backup; inspect prior patch state")
    backup.write_bytes(source.read_bytes())
    for old,new in changes.items(): text=text.replace(old,new,1)
    source.write_text(text)
elif not all(marker in text for marker in ('arc_generic_benchmark::initialize();','arc_generic_benchmark::tick();','arc_generic_benchmark::finish();')):
    raise RuntimeError("Incomplete existing generic harness")
shutil.copyfile(Path(__file__).resolve().parents[1]/"benchmarks/wicked/arc_generic_probe.h",directory/"arc_generic_probe.h")
print("Generic benchmark harness installed; host quality actions must be off")

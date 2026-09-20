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
shutil.copyfile(Path(__file__).resolve().parents[1]/"benchmarks/wicked/arc_dynamic_scene.h",directory/"arc_dynamic_scene.h")
tests=directory/'Tests.cpp'
test_text=tests.read_text(encoding="utf-8")
if '#include "arc_dynamic_scene.h"' not in test_text:
    marker='#include "ArcWickedHooks.h"'
    before='    wi::Timer arcScene; RenderPath3D::Update(dt);'
    if test_text.count(marker)!=1 or test_text.count(before)!=1: raise RuntimeError('Unexpected Tests.cpp motion hook shape')
    tests.with_suffix('.cpp.arc-motion-original').write_bytes(tests.read_bytes())
    test_text=test_text.replace(marker,marker+'\n#include "arc_dynamic_scene.h"',1)
    test_text=test_text.replace(before,'    arc_dynamic_scene::update(selected);\n'+before,1)
    tests.write_text(test_text,encoding="utf-8")

# Preserve local attribution additions: patch only the diagnostic scene loop.
bridge=directory/'ArcWickedBridge.cpp'
bridge_text=bridge.read_text(encoding="utf-8")
if 'ARC_WICKED_FOCUS_SCENE' not in bridge_text:
    bridge.with_suffix('.cpp.arc-focus-original').write_bytes(bridge.read_bytes())
    bridge_text=bridge_text.replace('        seconds_ = EnvInt(L"ARC_WICKED_SECONDS", 30, 10, 120);',
        '        focus_scene_ = EnvInt(L"ARC_WICKED_FOCUS_SCENE", -1, -1, 18);\n        if(focus_scene_!=-1&&focus_scene_!=0&&focus_scene_!=18){PostQuitMessage(2);return;}\n        seconds_ = EnvInt(L"ARC_WICKED_SECONDS", 30, 10, 120);',1)
    bridge_text=bridge_text.replace('    std::size_t scene_offset_ = 0;', '    int focus_scene_ = -1;\n    std::size_t scene_offset_ = 0;',1)
    start=bridge_text.index('    void ExperimentUpdate(');end=bridge_text.index('    void CaptureSceneSemantic(',start)
    loop=bridge_text[start:end]
    loop=loop.replace('        const auto now =', '        const std::size_t scene_count = focus_scene_>=0?1:kScenes.size();\n        const auto now =',1)
    loop=loop.replace('kScenes.size()', 'scene_count')
    loop=loop.replace('focus_scene_>=0?1:scene_count', 'focus_scene_>=0?1:kScenes.size()',1)
    loop=loop.replace('selector.SetSelected(kScenes[scene_offset_].combo_index);','selector.SetSelected(focus_scene_>=0?focus_scene_:kScenes[scene_offset_].combo_index);')
    loop=loop.replace('selector.SetSelected(kScenes[scene].combo_index);','selector.SetSelected(focus_scene_>=0?focus_scene_:kScenes[scene].combo_index);')
    loop=loop.replace('const auto scene = (desired + scene_offset_) % scene_count;','const auto scene = focus_scene_>=0?0:(desired + scene_offset_) % scene_count;')
    loop=loop.replace('<< kScenes[i].name <<', '<< (focus_scene_==0?"hello_world":focus_scene_==18?"instances_65k":kScenes[i].name) <<')
    bridge_text=bridge_text[:start]+loop+bridge_text[end:]
    bridge.write_text(bridge_text,encoding="utf-8")
print("Generic benchmark harness installed; host quality actions must be off")

"""Bounded, hidden DX12 startup/late-attach/child-handoff regression.

Usage: python connection_launch_tests.py <Release binaries> <package with runtime> <new evidence>
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

binary, package, evidence = map(lambda p: Path(p).resolve(), sys.argv[1:4])
evidence.mkdir(exist_ok=False)
scope = evidence / 'fixture'
scope.mkdir()
fixture = scope / 'arc-connection-native-tests.exe'
shutil.copy2(Path(sys.argv[4]) if len(sys.argv) > 4 else binary / fixture.name, fixture)
results = {}
for mode in ('late', 'direct', 'child', 'child-suspended'):
    case = evidence / mode
    case.mkdir()
    config = dict(target_fps=1, maximum_seconds=10, output=str(case / 'automatic'),
                  worker=str(binary / 'arc-shader-tool.exe'),
                  compiler=str(package / 'runtime/dxc/dxcompiler.dll'),
                  cache=str(evidence / 'shader-cache'), python=str(package / 'runtime/python/python.exe'),
                  critic=str(package / 'scripts/optimizer-live-quality.py'))
    if mode != 'late':
        config.update(launch_scope=str(scope), session_root=str(case),
                      launch_helper=str(binary / 'arc-dx12-probe-launch.exe'))
    config_path = case / 'config.json'
    config_path.write_text(json.dumps(config))
    env = os.environ.copy()
    for key in tuple(env):
        if key.startswith('ARC_'):
            env.pop(key)
    if mode == 'late':
        env.update(ARC_AUTO_CONFIG=str(config_path), ARC_OPTIMIZER_WORKER=config['worker'],
                   ARC_OPTIMIZER_COMPILER=config['compiler'], ARC_OPTIMIZER_CACHE=config['cache'])
        command = [str(fixture), '--render', str(case), str(binary / 'arc-dx12-probe.dll')]
    else:
        command = [str(binary / 'arc-dx12-probe-launch.exe'), '--launch-auto', str(fixture),
                   str(binary / 'arc-dx12-probe.dll'), str(case / 'arc.json'), str(config_path),
                   '--parent-suspended' if mode == 'child-suspended' else '--parent' if mode == 'child' else '--render', str(case)]
    run = subprocess.run(command, env=env, capture_output=True, timeout=50,
                         creationflags=subprocess.CREATE_NO_WINDOW)
    (case / 'launch-test.log').write_bytes(run.stdout + run.stderr)
    assert run.returncode == 0, (mode, run.stdout, run.stderr)
    deadline = time.monotonic() + 20
    while not (case / 'renderer.json').exists() and time.monotonic() < deadline:
        time.sleep(.05)
    renderer = json.loads((case / 'renderer.json').read_text())
    assert renderer['device_healthy']
    assert renderer['arc_before_main'] == (mode != 'late')
    metrics = case / 'arc.json'
    if mode.startswith('child'):
        parent = json.loads((case / 'parent.json').read_text())
        assert parent['renderer_pid'] == renderer['pid'] != parent['parent_pid']
        child = case / f"process-{renderer['pid']}"
        handoff = json.loads((child / 'handoff.json').read_text())
        assert handoff['initialized_before_resume'], handoff
        metrics = child / 'arc.json'
    data = json.loads(metrics.read_text())
    assert data['pid'] == renderer['pid'] and data['present_calls'] >= 100
    assert data['present_failures'] == data['hook_failures'] == 0
    coverage = data['optimizer_coverage']
    if mode == 'late':
        assert coverage['observed_root_signatures'] == coverage['observed_compute_pipelines'] == 0
        assert coverage['dispatch_declines']['root_unknown'] >= 100
        assert data['automatic_session']['phase'] == 'render_metadata_missing'
        assert not list((case / 'automatic').glob('trial-*')), 'late attach ran unrelated trials'
    else:
        assert coverage['observed_root_signatures'] >= 1, coverage
        assert coverage['observed_compute_pipelines'] >= 1, coverage
        assert coverage['dispatch_declines']['root_unknown'] == 0, coverage
    results[mode] = dict(pid=renderer['pid'], arc_before_main=renderer['arc_before_main'],
                         presents=data['present_calls'], roots=coverage['observed_root_signatures'],
                         pipelines=coverage['observed_compute_pipelines'],
                         unknown_roots=coverage['dispatch_declines']['root_unknown'],
                         phase=data['automatic_session']['phase'])
    print(mode, 'PASS', flush=True)
(evidence / 'summary.json').write_text(json.dumps(results, indent=2))

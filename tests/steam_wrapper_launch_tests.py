"""Test the ARC Steam-command entry point on an owned fixture, without Steam/game UI."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

package, fixture_source, evidence = map(lambda p: Path(p).resolve(), sys.argv[1:])
evidence.mkdir(exist_ok=False)
scope = evidence / 'fixture with spaces'
scope.mkdir()
fixture = scope / fixture_source.name
shutil.copy2(fixture_source, fixture)
for mode in ('direct', 'child'):
    case = evidence / mode
    case.mkdir()
    env = {k: v for k, v in os.environ.items() if not k.startswith('ARC_')}
    # Isolate the GUI's session files and use a marker, not a live Steam session.
    env.update(LOCALAPPDATA=str(case), SteamAppId='480')
    extra = ['space and "quote"', 'trailing\\'] if mode == 'direct' else []
    command = [str(package / 'arc-launcher.exe'), '--steam-launch', '1', str(fixture),
               '--render' if mode == 'direct' else '--parent', str(case), *extra]
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    wrapper = subprocess.Popen(command, env=env, startupinfo=startup)
    try:
        deadline = time.monotonic() + 45
        report = case / 'renderer.json'
        while not report.exists() and time.monotonic() < deadline:
            assert wrapper.poll() is None, 'wrapper exited before renderer'
            time.sleep(.1)
        renderer = json.loads(report.read_text())
        assert renderer['arc_before_main'] and renderer['device_healthy']
        assert renderer['steam_app_id'] == '480', 'Steam environment lost'
        if extra:
            assert renderer['arguments'][3:] == extra, renderer['arguments']
        matching = []
        for path in (case / 'ARC/sessions').rglob('arc.json'):
            data = json.loads(path.read_text())
            if data['pid'] == renderer['pid']:
                matching.append(data)
        assert len(matching) == 1
        data = matching[0]
        assert data['present_calls'] >= 100
        assert data['optimizer_coverage']['observed_compute_pipelines'] > 0
        assert data['optimizer_coverage']['dispatch_declines']['root_unknown'] == 0
        assert data['automatic_session']['target_fps'] == 1
        print(mode, 'Steam-wrapper entry PASS (synthetic context)', flush=True)
    finally:
        # Only the exact owned test wrapper is stopped; never enumerate/kill games.
        wrapper.terminate()
        wrapper.wait(timeout=5)

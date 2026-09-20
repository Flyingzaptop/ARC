"""Laboratory preflight only: no power/fan/clock settings are modified."""
import shutil
import subprocess
import time


def wait_for_cool_gpu(maximum_temperature, timeout=180):
    if maximum_temperature is None:
        return {'enabled': False}
    if not 40 <= maximum_temperature <= 80 or not 1 <= timeout <= 300:
        raise ValueError('Thermal gate requires 40..80 C and 1..300 seconds')
    if not shutil.which('nvidia-smi'):
        raise RuntimeError('Requested thermal gate requires nvidia-smi on this test machine')
    start = time.monotonic()
    samples = []
    ready = 0
    while time.monotonic()-start < timeout:
        result = subprocess.run(['nvidia-smi', '-i', '0',
            '--query-gpu=temperature.gpu,utilization.gpu,clocks.current.graphics,power.draw',
            '--format=csv,noheader,nounits'], capture_output=True, text=True, timeout=5,
            creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode:
            raise RuntimeError('GPU thermal telemetry unavailable')
        values = [float(x.strip()) for x in result.stdout.strip().split(',')]
        if len(values) != 4:
            raise RuntimeError('GPU thermal telemetry shape')
        sample = dict(zip(('temperature_c', 'gpu_utilization', 'graphics_mhz', 'power_w'), values))
        samples.append(sample)
        # Desktop compositing can report 20..35% activity at a very low clock.
        # Treat that as idle only with low power and clock; temperature still
        # must meet the unchanged start limit. A high-clock workload is refused.
        idle = values[1] < 15 or (values[2] <= 600 and values[3] < 25)
        ready = ready+1 if values[0] <= maximum_temperature and idle else 0
        if ready >= 3:
            return {'enabled': True, 'gpu_index': 0, 'maximum_start_temperature_c': maximum_temperature,
                    'wait_seconds': time.monotonic()-start, 'samples': samples}
        time.sleep(1)
    raise RuntimeError('GPU did not cool to the requested idle start condition; benchmark not launched')

import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'meter_summary', Path(__file__).parents[1] / 'scripts/arc2/summarize_cpu_meter.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SummaryTests(unittest.TestCase):
    def test_warmup_excluded_and_aggregate_cost_named(self):
        meter = dict(frame_columns=['swapchain', 'own_ns', 'excluded_native_ns', 'calls', 'qpc'],
                     frames=[[1, 0, 0, 0, 0], [1, 1000, 2000, 10, 10],
                             [1, 1500, 4000, 15, 20], [1, 3000, 8000, 30, 30]],
                     interpretation='aggregate wall time', sites=[['test', 3000, 30]],
                     dropped_frames=0, site_overflow=0)
        run = dict(measurement_start_qpc=10, measurement_end_qpc=20,
                   qpc_frequency=1000, frontend_sha256='diagnostic-only')
        result = module.summarize(meter, run)
        self.assertEqual(result['measured_present_intervals'], 1)
        self.assertEqual(result['aggregate_interceptor_wall_ms_per_present'], 0.0005)
        self.assertEqual(result['instrumented_calls_per_present'], 5)
        meter['frames'][2][0] = 2
        with self.assertRaisesRegex(ValueError, 'one measured swapchain'):
            module.summarize(meter, run)


if __name__ == '__main__':
    unittest.main()

"""Diagnostic arithmetic only; does not validate runtime offload admission."""
import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('screen', Path(__file__).with_name('offload-screen.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ScreenTest(unittest.TestCase):
    def test_nested_timing_and_unknown_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'screen.csv'
            with path.open('w', newline='') as stream:
                writer = csv.writer(stream)
                writer.writerow(['elapsed_ms', 'frame', 'event', 'ms'])
                for frame in range(20, 24):
                    for event, duration in [('SubmitCommandLists', 1), ('Application Update', 8),
                                            ('RenderPath3D Update', 5), ('Frustum Culling', 2)]:
                        writer.writerow([6000 + (frame-20)*40, frame, event, duration])
                writer.writerow([6000, 20, 'Frustum Culling', 99])
            result = module.screen(path)
        self.assertEqual(result['cpu_submit_cadence']['mean_ms'], 40)
        self.assertEqual(result['screen_gain_floor_ms'], 2)
        self.assertEqual(result['duplicate_event_frame_rows'], 1)
        candidates = {item['candidate']: item for item in result['ranked_candidates']}
        self.assertEqual(candidates['instance_update_residual']['wall']['mean_ms'], 3)
        self.assertEqual(candidates['visibility']['wall']['mean_ms'], 2)
        self.assertFalse(candidates['submission']['exceeds_screen_floor'])
        self.assertIsNone(candidates['visibility']['running_ms'])
        self.assertIsNone(candidates['visibility']['waiting_ms'])
        self.assertFalse(result['runtime_admission'])
        self.assertIsNone(result['control_drift_ms'])
        self.assertEqual(result['final_O2_gate'], 'not_evaluated')


if __name__ == '__main__':
    unittest.main()

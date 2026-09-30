import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'ir_summary', Path(__file__).parents[1] / 'scripts/arc2/summarize-ir.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SummaryTests(unittest.TestCase):
    def test_reachability_dedup_missing_history_and_unknowns(self):
        data = dict(work=[dict(id=1, access=[[7, 0, 0], [0, 0, 1, 0, 0, '', 3, 0, 2]]),
                          dict(id=2, access=[[0, 0, 2]])],
                    submissions=[[10, 4, [1, 100], [9], 1], [11, 4, [2], [10], 0]],
                    presents=[[0, 4, 0, 0, 0, 11, 0], [0, 4, 0, 0, 0, 11, 0]])
        result = module.semantic_counts(data)
        self.assertEqual(result['access_edges'], 3)
        self.assertEqual(result['symbolic_or_unknown_access_fraction'], 2 / 3)
        self.assertEqual(result['descriptor_interval_edges'], 1)
        self.assertEqual(result['present_linked_retained_work'], 2)
        self.assertEqual(result['present_linked_unretained_work_ids'], 1)
        self.assertEqual(result['missing_reachable_submission_ids'], 1)
        self.assertIsNone(result['dependency_closure_proven'])


if __name__ == '__main__':
    unittest.main()

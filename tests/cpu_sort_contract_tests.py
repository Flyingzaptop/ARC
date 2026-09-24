import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from cpu_sort_contract import preflight, compare_snapshot

BITS = list(range(64))
def record(k, payload=0):
    return k.to_bytes(8, 'little') + payload.to_bytes(8, 'little')

class SortContractTests(unittest.TestCase):
    def test_different_tie_permutation_is_not_visual_failure(self):
        a, b = record(1, 3), record(1, 4)
        result = compare_snapshot(a+b, b+a, BITS)
        self.assertTrue(result['differences_only_within_equal_key_groups'])
        self.assertFalse(result['byte_result_equal'])
        self.assertIsNone(result['observable_result_equal'])
        self.assertFalse(preflight(a+b, BITS)['input_guards_pass'])

    def test_identical_ties_are_byte_invariant_not_an_admission(self):
        result = preflight(record(1)*3, BITS)
        self.assertTrue(result['input_guards_pass'])
        self.assertFalse(result['replacement_admitted'])

    def test_unique_keys_with_minimum_first(self):
        result = preflight(record(1)+record(3)+record(2), BITS)
        self.assertTrue(result['input_guards_pass'])

    def test_minimum_and_path_size_guards(self):
        self.assertFalse(preflight(record(2)+record(1), BITS)['input_guards_pass'])
        self.assertFalse(preflight(record(1)*33, BITS)['input_guards_pass'])

    def test_pointer_wrap_alignment_and_truncation(self):
        for data, begin, end in [(b'x',0,1), (record(1),1,17),
                                 (record(1),(1<<64)-16,1<<64), (record(1),0,32)]:
            self.assertFalse(preflight(data,BITS,begin,end)['input_guards_pass'])

    def test_unsigned_high_bit_and_corruption(self):
        a = record(0)+record(1<<63)
        self.assertTrue(compare_snapshot(a,a,BITS)['nondecreasing_unsigned_keys'])
        self.assertFalse(compare_snapshot(a,record(0)*2,BITS)['same_multiset'])

    def test_invalid_projection(self):
        for bits in [[0], [128]*64, [True]*64]:
            with self.assertRaises(ValueError):preflight(record(1),bits)

if __name__ == '__main__':unittest.main()

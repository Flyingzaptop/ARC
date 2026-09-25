import sys,json,hashlib,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
from cpu_large_sort import exact_sort
ROOT=Path(__file__).resolve().parents[1]
BITS=list(range(64,80))+list(range(96,104))+list(range(16))+list(range(104,128))

def encode(k,identifier):
    value=identifier<<32
    for pos,b in enumerate(BITS):value=(value&~(1<<b))|(((k>>pos)&1)<<b)
    return value.to_bytes(16,'little')

class LargeSortTests(unittest.TestCase):
    def test_against_saved_native_hashes_with_distinct_tie_payloads(self):
        results=json.loads((ROOT/'docs/evidence/cpu-large-admission-20260925/large-native-oracle.json').read_text())
        reference={x['name']:x for x in results['cases']}
        for pattern in ['two-keys','descending','organ-pipe']:
            n=4096
            keys=[i%2 for i in range(n)] if pattern=='two-keys' else list(range(n,0,-1)) if pattern=='descending' else [min(i,n-1-i) for i in range(n)]
            data=b''.join(encode(k,i) for i,k in enumerate(keys))
            result,stats=exact_sort(data,BITS)
            self.assertEqual(hashlib.sha256(result).hexdigest(),reference[f'{pattern}-{n}']['output_sha256'])
            self.assertGreater(stats['partitions'],0)
    def test_forced_heap_tie_order_matches_native(self):
        results=json.loads((ROOT/'docs/evidence/cpu-large-admission-20260925/large-native-oracle.json').read_text())
        ref=next(x for x in results['cases'] if x['name']=='forced-heap-two-keys-257')
        result,stats=exact_sort(b''.join(encode(i%2,i) for i in range(257)),BITS,0)
        self.assertEqual(stats['heap_ranges'],1)
        self.assertEqual(hashlib.sha256(result).hexdigest(),ref['output_sha256'])
    def test_input_bounds(self):
        for data,budget in [(b'x',0),(b'\0'*(4*1024*1024+16),0),(b'\0'*64,-1),(b'\0'*64,5)]:
            with self.assertRaises(ValueError):exact_sort(data,BITS,budget)
if __name__=='__main__':unittest.main()

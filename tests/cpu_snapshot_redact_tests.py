import json
import hashlib
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from cpu_snapshot_redact import Marker, RedactionError, _copy_marked, bind_original_hashes, rebase_native_manifest
from cpu_snapshot_gather import packet_proofs as append_proof
from cpu_word_snapshot import packet_proofs as word_proof

OVERRIDE=os.environ.get('ARC_COMPOSITE_REDACTED_ROOT')
BASE=Path(OVERRIDE) if OVERRIDE else ROOT/'build/cpu-ir-gpu'
EVIDENCE_OVERRIDE=os.environ.get('ARC_COMPOSITE_EVIDENCE_ROOT')
WORD=(Path(EVIDENCE_OVERRIDE)/'cpu-composite-pack-bulk-20260927/candidate-00'
      if EVIDENCE_OVERRIDE and not OVERRIDE else BASE/'redacted-word')
FILTER=(Path(EVIDENCE_OVERRIDE)/'cpu-composite-bulk-covered-v2-20260927/candidate-01'
        if EVIDENCE_OVERRIDE and not OVERRIDE else BASE/'redacted-filter-v2')


class RedactionTests(unittest.TestCase):
    def require(self,path):
        if path.exists():return
        message=f'missing {path}; set ARC_COMPOSITE_REDACTED_ROOT to extracted redacted captures'
        if OVERRIDE or EVIDENCE_OVERRIDE:self.fail(message)
        self.skipTest(message)

    def test_exact_intervals_no_hull(self):
        marker=Marker([{'base':100,'size':12}])
        marker.mark(102,2);marker.mark(108,1)
        self.assertEqual(marker.counts(),[3])
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source=root/'source.bin';target=root/'target.bin'
            source.write_bytes(b'abcdefghijkl')
            _copy_marked(source,target,12,marker.masks[0])
            self.assertEqual(target.read_bytes(),b'\0\0cd\0\0\0\0i\0\0\0')

    def test_rebase_rejects_original(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp)
            (root/'native-manifest.json').write_text('{}')
            with self.assertRaisesRegex(RedactionError,'unmarked original'):
                rebase_native_manifest(root)

    def test_hash_binding_keeps_original_unchanged(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);original=root/'original';redacted=root/'redacted'
            original.mkdir();redacted.mkdir()
            (original/'before.bin').write_bytes(b'abcdefgh')
            (original/'after.bin').write_bytes(b'ABCDEFGH')
            views={'regions':[{'base':100,'size':8,'before_file':'before.bin','after_file':'after.bin'}]}
            (original/'views.json').write_text(json.dumps(views))
            marker=Marker(views['regions']);marker.mark(102,2)
            before_hash=_copy_marked(original/'before.bin',redacted/'before.bin',8,marker.masks[0])
            after_hash=_copy_marked(original/'after.bin',redacted/'after.bin',8,marker.masks[0])
            report={'status':'exact_range_redacted_snapshot','source_candidate':str(original),
                    'regions':[{'base':100,'size':8,'before_sha256':before_hash,'after_sha256':after_hash}]}
            (redacted/'redaction.json').write_text(json.dumps(report))
            updated=bind_original_hashes(redacted)
            self.assertTrue(updated['original_hashes_bound'])
            self.assertEqual(updated['regions'][0]['before_original_sha256'],hashlib.sha256(b'abcdefgh').hexdigest())
            self.assertEqual((original/'before.bin').read_bytes(),b'abcdefgh')
            self.assertEqual((redacted/'before.bin').read_bytes(),b'\0\0cd\0\0\0\0')

    def test_word_redacted_proof(self):
        candidate=WORD;self.require(candidate/'redaction.json')
        report=json.loads((candidate/'redaction.json').read_text())
        self.assertEqual(report['status'],'exact_range_redacted_snapshot')
        self.assertTrue(report['original_hashes_bound'])
        self.assertLess(report['total_before_retained_bytes'],10_000_000)
        self.assertEqual(report['total_after_retained_bytes'],261390)
        with tempfile.TemporaryDirectory() as temp:
            proof=word_proof(candidate,Path(temp)/'proof')
            self.assertEqual(proof['expected_words']['sha256'],
                             '0f4f7fb207622c348fc0da9038c2bb3cd8e3d4fb897f1c18331cf5eb7f6d6d7c')

    def test_append_redacted_proof(self):
        candidate=FILTER;self.require(candidate/'redaction.json')
        report=json.loads((candidate/'redaction.json').read_text())
        self.assertEqual(report['status'],'exact_range_redacted_snapshot')
        self.assertTrue(report['original_hashes_bound'])
        self.assertLess(report['total_before_retained_bytes'],10_000_000)
        self.assertEqual(report['total_after_retained_bytes'],1045528)
        proof=append_proof(candidate,candidate/'native-manifest.json')
        self.assertEqual(proof['independent_after_view']['record_sha256'],
                         '1ef5d0b40aa3c4be080f956b1cd3e3292e70c7ca27dc0628ecf3bc1498932c3a')


if __name__=='__main__':unittest.main()

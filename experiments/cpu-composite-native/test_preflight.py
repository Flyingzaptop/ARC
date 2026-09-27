"""Small negative gates for the captured isolated replay manifest."""
import json
import os
import sys
import unittest
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from preflight import audit_manifest
from prepare import prepare_pack

EVIDENCE_OVERRIDE = os.environ.get('ARC_COMPOSITE_EVIDENCE_ROOT')
MANIFEST = (Path(EVIDENCE_OVERRIDE) /
            'cpu-composite-bulk-covered-v2-20260927' /
            'candidate-01' / 'native-manifest.json') if EVIDENCE_OVERRIDE else None


class NativePreflightTests(unittest.TestCase):
    def setUp(self):
        if MANIFEST is None:
            self.skipTest('set ARC_COMPOSITE_EVIDENCE_ROOT to extracted captures')
        if not MANIFEST.is_file():
            self.fail(f'missing {MANIFEST} under ARC_COMPOSITE_EVIDENCE_ROOT')
        self.source = json.loads(MANIFEST.read_text())
        base = MANIFEST.parent
        for key in ('entry_context_file', 'exit_context_file'):
            if key in self.source:
                self.source[key] = str((base / self.source[key]).resolve())
        for region in self.source['regions']:
            for key in ('before_file', 'after_file'):
                region[key] = str((base / region[key]).resolve())
        for key in ('capture', 'append_contract', 'module_image'):
            self.source[key] = str((base / self.source[key]).resolve())
        self.path = HERE / ('test-manifest-' + uuid.uuid4().hex + '.json')
        self.output = HERE / ('test-pack-' + uuid.uuid4().hex + '.bin')

    def tearDown(self):
        if hasattr(self, 'path'):
            self.path.unlink(missing_ok=True)
            self.output.unlink(missing_ok=True)

    def _audit(self):
        self.path.write_text(json.dumps(self.source))
        return audit_manifest(self.path)

    def test_short_snapshot_region_is_rejected(self):
        self.source['regions'][0]['size'] = 8
        result = self._audit()
        self.assertFalse(result['static_preflight_pass'])
        self.assertIn('snapshot_does_not_cover_observed_memory', result['errors'])

    def test_wrong_captured_gs_value_is_rejected_before_pack(self):
        self.source['teb_tls_pointer_value'] ^= 1
        self.assertTrue(self._audit()['static_preflight_pass'])
        with self.assertRaisesRegex(ValueError, 'captured_gs_pointer_changed'):
            prepare_pack(self.path, self.output)
        self.assertFalse(self.output.exists())

    def test_declared_trap_set_cannot_omit_alternate_calls(self):
        self.source['trap_call_rvas'] = []
        result = self._audit()
        self.assertFalse(result['static_preflight_pass'])
        self.assertIn('untrapped_out_of_span_call', result['errors'])
        self.assertTrue(result['required_trap_call_rvas'])


if __name__ == '__main__':
    unittest.main()

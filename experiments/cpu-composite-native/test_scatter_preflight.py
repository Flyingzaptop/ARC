"""Focused static negatives for the bulk word-scatter pack."""
import copy
import json
import os
import sys
import unittest
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from prepare_scatter import prepare_scatter_pack

EVIDENCE_OVERRIDE = os.environ.get('ARC_COMPOSITE_EVIDENCE_ROOT')
ARTIFACT_OVERRIDE = os.environ.get('ARC_COMPOSITE_ARTIFACT_ROOT')
BASE_MANIFEST = (Path(EVIDENCE_OVERRIDE) /
                 'cpu-composite-pack-bulk-20260927' /
                 'candidate-00' / 'scatter-manifest.json') if EVIDENCE_OVERRIDE else None


class ScatterPreflightTests(unittest.TestCase):
    def setUp(self):
        if BASE_MANIFEST is None:
            self.skipTest('set ARC_COMPOSITE_EVIDENCE_ROOT to extracted captures')
        if not BASE_MANIFEST.is_file():
            self.fail(f'missing {BASE_MANIFEST} under ARC_COMPOSITE_EVIDENCE_ROOT')
        if not ARTIFACT_OVERRIDE:
            self.fail('set ARC_COMPOSITE_ARTIFACT_ROOT to extracted artifacts/cpu-ir-gpu')
        artifact = Path(ARTIFACT_OVERRIDE) / 'grouped-mask-contract-bulk2.json'
        if not artifact.is_file():
            self.fail(f'missing {artifact} under ARC_COMPOSITE_ARTIFACT_ROOT')
        self.manifest = copy.deepcopy(json.loads(BASE_MANIFEST.read_text()))
        base = BASE_MANIFEST.parent
        for key in ('capture', 'module_image', 'entry_context_file', 'exit_context_file'):
            self.manifest[key] = str((base / self.manifest[key]).resolve())
        for region in self.manifest['regions']:
            for key in ('before_file', 'after_file'):
                region[key] = str((base / region[key]).resolve())
        self.manifest['word_contract'] = str(artifact.resolve())
        suffix = uuid.uuid4().hex
        self.temp_manifest = HERE / ('test-scatter-manifest-' + suffix + '.json')
        self.temp_contract = HERE / ('test-scatter-contract-' + suffix + '.json')
        self.output = HERE / ('test-scatter-pack-' + suffix + '.bin')

    def tearDown(self):
        if hasattr(self, 'temp_manifest'):
            for path in (self.temp_manifest, self.temp_contract, self.output):
                path.unlink(missing_ok=True)

    def _prepare(self):
        self.temp_manifest.write_text(json.dumps(self.manifest))
        return prepare_scatter_pack(self.temp_manifest, self.output)

    def test_bulk_contract_resolves_word_span_and_no_gs(self):
        result = self._prepare()
        self.assertEqual(result['natural_iterations'], 65343)
        self.assertEqual(result['output_words'], 65343)
        self.assertEqual(result['output_protection'], 0x404)
        self.assertEqual(result['trapped_call_rvas'], [0x1cbc55])
        self.assertTrue(result['full_output_overwrite_proven'])
        self.assertFalse(result['native_execution_started'])

    def test_writecombine_downgrade_is_rejected(self):
        self.manifest['regions'][-1]['protect'] = 0x4
        with self.assertRaisesRegex(ValueError, 'writecombine_output_span_not_captured'):
            self._prepare()
        self.assertFalse(self.output.exists())

    def test_store_machine_byte_change_is_rejected(self):
        contract = json.loads(Path(self.manifest['word_contract']).read_text())
        contract['word_scatter']['store_code'] = '90'
        self.temp_contract.write_text(json.dumps(contract))
        self.manifest['word_contract'] = str(self.temp_contract)
        with self.assertRaisesRegex(ValueError, 'scatter_store_machine_bytes_changed'):
            self._prepare()
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()

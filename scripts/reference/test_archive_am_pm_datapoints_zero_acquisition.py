"""Committed acquisition provenance guards, with no local raw files or SystemVue."""

import copy
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import shutil
import tempfile
import unittest


SCRIPT = Path(__file__).with_name('archive-am-pm-datapoints-zero-acquisition.py')
ROOT = SCRIPT.resolve().parents[2]
PLAN = 'validation/am-pm-datapoints-zero-plan-20261013.json'
MANIFEST = 'validation/am-pm-datapoints-zero-captures-20261013.json'
OBSERVATIONS = 'validation/am-pm-datapoints-zero-observations-20261013.json'
REPAIRS = 'validation/am-pm-datapoints-zero-workflow-repairs-20261013.json'
RESTORATION = 'validation/am-pm-datapoints-zero-restoration-20261013.json'
BASELINES = (
    'validation/am-pm-datapoints-observations-20261012.json',
    'validation/am-pm-existing-reference-evidence-20261012.json',
)
COMMITTED_FIXTURES = (PLAN, MANIFEST, OBSERVATIONS, REPAIRS, RESTORATION) + BASELINES
PROFILES = (
    'original-triangle', 'first-knot-negative', 'first-knot-positive',
    'zero-interior-knot', 'constant-phase-loss', 'unity-gain-control',
    'dense-zero-neighborhood',
)
PLAN_SHA256 = '0ee1496412224c03daebb746db559435a2e4244b446180d913b67e2c8cd62af0'
# Measured once from the seven original files, independently of the manifest
# builder. These identify the archive; this suite does not possess raw bytes.
CAPTURE_SHA256 = (
    '1e533e8cbd646b59d50b8a145afed919d60603c1ce2b35ced4167e214c9db878',
    '04e9aecc0edabeb84240a65a8223049ba45b3f7536c107d5fe54f9ce5640ff9f',
    'f930126a26bd5b9f830bd58e5a71c17a4c545f7bc6e8f284c882dd98bf62d078',
    '51b9bcdc296924643de7b4a2d58ae50b80b04f6b7c33d46da62beb8e640da886',
    '4e2da3365290a09076e6ba2ef51ab2eb232b1ee157ec38a7ab7d80c7367215a8',
    'f7930450f301bbf5074c2fafaedc9eb899fab8af9717c7e74fbd1a8c0f157606',
    'ff224b3313f45cec58fd197df5304183c8f23e3c8efb5928e407d7abf8af1830',
)

spec = importlib.util.spec_from_file_location('zero_acquisition_archive', SCRIPT)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def serialized_digest(value):
    return hashlib.sha256((json.dumps(value, indent=2, allow_nan=False) + '\n').encode('utf-8')).hexdigest()


def write(path, value):
    with Path(path).open('w', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')


class DataPointsZeroAcquisitionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for relative in COMMITTED_FIXTURES:
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, destination)
        self.plan = load(self.root / PLAN)
        self.manifest = load(self.root / MANIFEST)
        self.observations = load(self.root / OBSERVATIONS)

    def verify(self):
        return m.verify_committed(self.root, self.root / PLAN,
                                  self.root / MANIFEST, self.root / OBSERVATIONS)

    def reject(self):
        with self.assertRaises((ValueError, FileNotFoundError)):
            self.verify()

    def write_manifest(self, manifest, observations=None):
        # Deliberately repair the outer link after a manifest mutation. A
        # semantic test must not pass solely because the old envelope hash fails.
        write(self.root / MANIFEST, manifest)
        observations = copy.deepcopy(self.observations if observations is None else observations)
        observations['capture_manifest_sha256'] = digest(self.root / MANIFEST)
        write(self.root / OBSERVATIONS, observations)

    def edit_capture_source(self, update_source_hash=False):
        manifest = copy.deepcopy(self.manifest)
        old_hash = manifest['captures'][0]['validation']['artifact_sha256']['source-snapshot.json']
        snapshot = json.loads(manifest['source_snapshots'][old_hash]['utf8_json'])
        helpers = [source for path, source in snapshot.items()
                   if path.replace('\\', '/').endswith('/capture.ps1')]
        self.assertEqual(len(helpers), 1)
        helpers[0]['utf8_source'] += '\n# edited archived execution source\n'
        if update_source_hash:
            helpers[0]['sha256'] = hashlib.sha256(helpers[0]['utf8_source'].encode('utf-8')).hexdigest()
        text = json.dumps(snapshot, indent=2, allow_nan=False) + '\n'
        new_hash = hashlib.sha256(text.encode('utf-8')).hexdigest()
        del manifest['source_snapshots'][old_hash]
        manifest['source_snapshots'][new_hash] = {'utf8_json': text}
        for row in [manifest['park']] + manifest['captures'] + [manifest['restore']]:
            artifacts = row['validation']['artifact_sha256']
            if artifacts['source-snapshot.json'] == old_hash:
                artifacts['source-snapshot.json'] = new_hash
                row['validated_sha256'] = serialized_digest(row['validation'])
        self.write_manifest(manifest)

    def test_fixture_contains_only_committed_json_and_frozen_plan(self):
        self.assertEqual(sorted(path.relative_to(self.root).as_posix()
                                for path in self.root.rglob('*') if path.is_file()),
                         sorted(COMMITTED_FIXTURES))
        self.assertFalse((self.root / 'build-reference').exists())
        self.assertEqual(digest(self.root / PLAN), PLAN_SHA256)
        self.assertEqual(tuple(profile['name'] for profile in self.plan['profiles']), PROFILES)
        self.assertEqual(sum(profile['expected_points'] for profile in self.plan['profiles']), 387)

    def test_committed_chain_verifies_without_raw_files_or_mutating_evidence(self):
        before = {relative: (self.root / relative).read_bytes()
                  for relative in COMMITTED_FIXTURES}
        result = self.verify()
        self.assertIsInstance(result, dict)
        self.assertIs(result['committed_archive_verified'], True)
        self.assertIs(result['raw_source_bytes_verified'], False)
        self.assertEqual({key: result[key] for key in ('capture_count', 'planned_sweep_points',
                                                      'ordinary_points', 'extra_sweep_points')},
                         dict(capture_count=7, planned_sweep_points=387,
                              ordinary_points=7, extra_sweep_points=7))
        self.assertFalse((self.root / 'build-reference').exists())
        self.assertEqual({relative: (self.root / relative).read_bytes()
                          for relative in COMMITTED_FIXTURES}, before)

    def test_seven_original_capture_identities_and_observation_links_are_retained(self):
        rows, samples = self.manifest['captures'], self.observations['samples']
        self.assertEqual(tuple(row['profile'] for row in rows), PROFILES)
        self.assertEqual(tuple(row['validation']['artifact_sha256']['capture.json'] for row in rows),
                         CAPTURE_SHA256)
        self.assertEqual(len({row['directory'] for row in rows}), 7)
        for row, sample in zip(rows, samples):
            with self.subTest(profile=row['profile']):
                self.assertEqual(sample['capture_source'], dict(
                    directory=row['directory'],
                    capture_sha256=row['validation']['artifact_sha256']['capture.json'],
                    normalized_sha256=row['validation']['artifact_sha256']['normalized.json'],
                    sample_sha256=row['sample_sha256']))
                normalized = {key: value for key, value in sample.items() if key != 'capture_source'}
                encoded = json.dumps(normalized, sort_keys=True, separators=(',', ':'),
                                     allow_nan=False).encode('utf-8')
                self.assertEqual(hashlib.sha256(encoded).hexdigest(), row['sample_sha256'])
                self.assertEqual(serialized_digest(normalized),
                                 row['validation']['artifact_sha256']['normalized.json'])

    def test_real_samples_preserve_all_profiles_registered_points_and_extras(self):
        samples = self.observations['samples']
        self.assertEqual(tuple(sample['profile']['name'] for sample in samples), PROFILES)
        self.assertEqual([len(sample['sweep']) for sample in samples], [61] * 6 + [21])
        self.assertEqual([len(sample['additional_sweep_points']) for sample in samples], [1] * 7)
        self.assertEqual(sum(len(sample['ordinary']['raw_carrier']) for sample in samples), 14)
        for profile, sample in zip(self.plan['profiles'], samples):
            with self.subTest(profile=profile['name']):
                self.assertEqual(sample['profile'], profile)
                self.assertEqual([point['index'] for point in sample['sweep']],
                                 list(range(profile['expected_points'])))
                self.assertEqual(sample['additional_sweep_points'][0]['index'],
                                 profile['expected_points'])

    def test_frozen_plan_byte_edit_rejected_even_with_identical_json(self):
        path = self.root / PLAN
        path.write_bytes(path.read_bytes() + b'\n')
        self.reject()

    def test_baseline_files_are_required_and_checked_byte_for_byte(self):
        for relative in BASELINES:
            path = self.root / relative
            original = path.read_bytes()
            with self.subTest(path=relative, mutation='missing'):
                path.unlink()
                self.reject()
                path.write_bytes(original)
            with self.subTest(path=relative, mutation='same JSON different bytes'):
                path.write_bytes(original + b'\n')
                self.reject()
                path.write_bytes(original)

    def test_repair_and_restoration_proofs_are_required_and_hash_bound(self):
        for relative in (REPAIRS, RESTORATION):
            path = self.root / relative
            original = path.read_bytes()
            with self.subTest(path=relative, mutation='missing'):
                path.unlink()
                self.reject()
                path.write_bytes(original)
            with self.subTest(path=relative, mutation='same JSON different bytes'):
                path.write_bytes(original + b'\n')
                self.reject()
                path.write_bytes(original)

    def test_observation_profile_order_and_completeness_cannot_change(self):
        for kind in ('reverse', 'remove', 'duplicate'):
            evidence = copy.deepcopy(self.observations)
            if kind == 'reverse':
                evidence['samples'].reverse()
            elif kind == 'remove':
                evidence['samples'].pop()
            else:
                evidence['samples'][1] = copy.deepcopy(evidence['samples'][0])
            with self.subTest(mutation=kind):
                write(self.root / OBSERVATIONS, evidence)
                self.reject()

    def test_registered_sweep_cannot_drop_duplicate_or_reorder_near_zero_points(self):
        for kind in ('drop', 'duplicate', 'reorder'):
            evidence = copy.deepcopy(self.observations)
            sweep = evidence['samples'][-1]['sweep']
            if kind == 'drop':
                sweep.pop(11)
            elif kind == 'duplicate':
                sweep[11] = copy.deepcopy(sweep[10])
            else:
                sweep[10], sweep[11] = sweep[11], sweep[10]
            with self.subTest(mutation=kind):
                write(self.root / OBSERVATIONS, evidence)
                self.reject()

    def test_retained_extra_endpoint_cannot_be_dropped_or_changed(self):
        for kind in ('drop', 'phase edit'):
            evidence = copy.deepcopy(self.observations)
            extra = evidence['samples'][-1]['additional_sweep_points']
            if kind == 'drop':
                extra.clear()
            else:
                extra[0]['output_phase_rad'] += .01
            with self.subTest(mutation=kind):
                write(self.root / OBSERVATIONS, evidence)
                self.reject()

    def test_observed_power_and_phase_cannot_be_replaced_by_fitted_values(self):
        for field, value in (('output_power_w', .001000230285),
                             ('output_phase_rad', math.radians(10))):
            evidence = copy.deepcopy(self.observations)
            evidence['samples'][-1]['sweep'][11][field] = value
            with self.subTest(field=field):
                write(self.root / OBSERVATIONS, evidence)
                self.reject()

    def test_manifest_byte_link_cannot_be_silently_replaced(self):
        path = self.root / MANIFEST
        path.write_bytes(path.read_bytes() + b'\n')
        with self.assertRaisesRegex(ValueError, 'Capture manifest identity changed'):
            self.verify()

    def test_prepared_workspace_manifest_is_bound_to_its_recorded_file_digest(self):
        for kind in ('digest', 'workspace edit'):
            manifest = copy.deepcopy(self.manifest)
            if kind == 'digest':
                manifest['prepared_manifest_sha256'] = '0' * 64
            else:
                manifest['prepared']['profiles'][0]['changes'][0]['after'] = '999'
            with self.subTest(mutation=kind):
                self.write_manifest(manifest)
                with self.assertRaises(ValueError):
                    self.verify()

    def test_validation_record_is_bound_to_its_recorded_file_digest(self):
        for kind in ('digest', 'validation time'):
            manifest = copy.deepcopy(self.manifest)
            if kind == 'digest':
                manifest['captures'][0]['validated_sha256'] = '0' * 64
            else:
                # Numerically equivalent timestamp spelling still changes the
                # original recorded JSON bytes, without breaking chronology.
                value = manifest['captures'][0]['validation']['validated_utc']
                self.assertTrue(value.endswith('+00:00'))
                manifest['captures'][0]['validation']['validated_utc'] = value[:-6] + 'Z'
            with self.subTest(mutation=kind):
                self.write_manifest(manifest)
                with self.assertRaises(ValueError):
                    self.verify()

    def test_baseline_pass_flag_cannot_override_replayed_residuals(self):
        manifest = copy.deepcopy(self.manifest)
        self.assertIs(manifest['baseline']['rows'][0]['matched'], True)
        manifest['baseline']['rows'][0]['phase_circular_error_deg'] = 100
        self.write_manifest(manifest)
        with self.assertRaises(ValueError):
            self.verify()

    def test_rehashed_sample_must_still_match_original_normalized_file_digest(self):
        manifest, evidence = copy.deepcopy(self.manifest), copy.deepcopy(self.observations)
        sample = evidence['samples'][-1]
        sample['sweep'][11]['output_phase_rad'] = math.radians(10)
        normalized = {key: value for key, value in sample.items() if key != 'capture_source'}
        encoded = json.dumps(normalized, sort_keys=True, separators=(',', ':'),
                             allow_nan=False).encode('utf-8')
        sample_hash = hashlib.sha256(encoded).hexdigest()
        manifest['captures'][-1]['sample_sha256'] = sample_hash
        sample['capture_source']['sample_sha256'] = sample_hash
        self.write_manifest(manifest, evidence)
        with self.assertRaisesRegex(ValueError, 'normalized|Normalized'):
            self.verify()

    def test_embedded_execution_status_must_match_original_status_file_digest(self):
        manifest = copy.deepcopy(self.manifest)
        manifest['captures'][0]['status']['helper_pid'] += 1
        self.write_manifest(manifest)
        with self.assertRaisesRegex(ValueError, 'status|Status'):
            self.verify()

    def test_embedded_source_edit_is_detected_after_rehashing_snapshot_and_manifest(self):
        self.edit_capture_source()
        with self.assertRaisesRegex(ValueError, 'Embedded execution source bytes changed'):
            self.verify()

    def test_rehashed_source_must_still_match_the_executed_helper_identity(self):
        self.edit_capture_source(update_source_hash=True)
        with self.assertRaisesRegex(ValueError, 'Executed helper is not the archived source'):
            self.verify()

    def test_missing_linked_execution_source_is_rejected_after_rehashing_manifest(self):
        manifest = copy.deepcopy(self.manifest)
        source_hash = manifest['captures'][0]['validation']['artifact_sha256']['source-snapshot.json']
        del manifest['source_snapshots'][source_hash]
        self.write_manifest(manifest)
        with self.assertRaisesRegex(ValueError, 'Missing linked execution source snapshot'):
            self.verify()

    def test_capture_source_hashes_cannot_be_mixed_between_profiles(self):
        for field in ('directory', 'capture_sha256', 'normalized_sha256', 'sample_sha256'):
            evidence = copy.deepcopy(self.observations)
            evidence['samples'][0]['capture_source'][field] = evidence['samples'][1]['capture_source'][field]
            with self.subTest(field=field):
                write(self.root / OBSERVATIONS, evidence)
                with self.assertRaisesRegex(ValueError, 'Sample raw-source link changed'):
                    self.verify()

    def test_count_and_raw_verification_claims_cannot_be_inflated(self):
        changes = [('analysis_calls', 8), ('analysis_calls', True),
                   ('reserved_analysis_calls_used', 1), ('planned_sweep_points', 394),
                   ('ordinary_points', 8), ('extra_sweep_points', 0),
                   ('raw_source_bytes_verified_at_collection', False),
                   ('raw_source_bytes_verified_at_collection', 1),
                   ('vendor_workspace_bytes_embedded', True)]
        for field, value in changes:
            manifest = copy.deepcopy(self.manifest)
            manifest[field] = value
            with self.subTest(field=field, value=value):
                self.write_manifest(manifest)
                with self.assertRaisesRegex(ValueError, 'Acquisition counts changed|Archive scope changed'):
                    self.verify()

    def test_later_analysis_cannot_begin_before_predecessor_validation(self):
        manifest = copy.deepcopy(self.manifest)
        row = manifest['captures'][1]
        row['status']['started_utc'] = manifest['captures'][0]['completion']['run_started_utc']
        row['validation']['artifact_sha256']['status.json'] = serialized_digest(row['status'])
        row['validated_sha256'] = serialized_digest(row['validation'])
        self.write_manifest(manifest)
        with self.assertRaisesRegex(ValueError, 'Next call started before predecessor validation or completion'):
            self.verify()

    def test_incomplete_or_diagnostic_run_cannot_enter_archive(self):
        for field, value in (('analysis_done', 0), ('analysis_done', True),
                             ('manager_errors', '(WARNING) modified analysis')):
            manifest = copy.deepcopy(self.manifest)
            manifest['captures'][0]['completion'][field] = value
            with self.subTest(field=field, value=value):
                self.write_manifest(manifest)
                with self.assertRaisesRegex(ValueError, 'Analysis incomplete or diagnostic'):
                    self.verify()

    def test_full_raw_verification_cannot_succeed_from_committed_hashes_alone(self):
        self.assertFalse((self.root / 'build-reference').exists())
        with self.assertRaises((ValueError, FileNotFoundError)):
            m.verify_archive(self.root, self.root / PLAN,
                             self.root / MANIFEST, self.root / OBSERVATIONS)


if __name__ == '__main__':
    unittest.main()

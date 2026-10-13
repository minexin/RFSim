"""Synthetic workflow guards and committed acquisition replay; no vendor calls."""

import copy
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name('archive-am-pm-datapoints-allocation-acquisition.py')
ROOT = SCRIPT.resolve().parents[2]
PLAN = ROOT / 'validation/am-pm-datapoints-allocation-plan-20261014.json'
MANIFEST = 'validation/am-pm-datapoints-allocation-captures-20261014.json'
OBSERVATIONS = 'validation/am-pm-datapoints-allocation-observations-20261014.json'
FIXTURES = ('validation/am-pm-datapoints-allocation-plan-20261014.json',
            'validation/am-pm-datapoints-allocation-workflow-20261014.json', MANIFEST, OBSERVATIONS,
            'validation/am-pm-datapoints-raw-axis-observations-20261013.json')
spec = importlib.util.spec_from_file_location('allocation_acquisition', SCRIPT)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def sha(payload):
    return hashlib.sha256(payload).hexdigest()


def encoded(value):
    return (json.dumps(value, indent=2, allow_nan=False) + '\n').encode('utf-8')


def workflow_fixture(plan):
    """Frozen preacquisition configuration, containing no vendor measurements."""
    return m.load(ROOT / m.WORKFLOW)


def parameter_nodes(profile):
    nodes = []
    for path, value in m.normalizer.expected_parameters(profile).items():
        nodes.append(dict(path=path, data=copy.deepcopy(value),
                          dimensions=[1, len(value)] if isinstance(value, list) else None,
                          data_entry=m.preparer.source_expression(profile['ordinary_output_target_dbm'], profile['gain_db'])
                          if path == m.SOURCE_POWER else m.preparer.gain_expression(profile['gain_db'])
                          if path == m.GAIN else None))
    nodes.append(dict(path=m.ENABLE, data=1, dimensions=None))
    return nodes


class AllocationAcquisitionGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.plan = json.loads(PLAN.read_text(encoding='utf-8-sig'))

    def setUp(self):
        self.profile = self.plan['profiles'][0]
        self.nodes = parameter_nodes(self.profile)
        self.workflow = workflow_fixture(self.plan)

    def test_frozen_plan_has_twelve_calls_and_eighteen_readbacks(self):
        self.assertEqual(sha(PLAN.read_bytes()), m.PLAN_SHA256)
        self.assertEqual(m.PLAN_SHA256, m.preparer.PLAN_SHA256)
        self.assertEqual(len(self.plan['profiles']), 12)
        self.assertEqual(sum(profile['expected_points'] for profile in self.plan['profiles']), 732)
        values = m.readbacks(self.nodes, self.profile)
        self.assertEqual(len(values), 18)
        self.assertEqual(values[m.ENABLE], dict(data=1, dimensions=None, data_entry=None))
        self.assertEqual(values[m.normalizer.SOURCE + 'Pwr']['data'], .00001)
        self.assertEqual(values[m.normalizer.AMP + 'G']['data'], 100)
        self.assertAlmostEqual(values[m.normalizer.SOURCE + 'Phase']['data'], math.radians(37))
        m.check_readbacks(values, self.profile)

    def test_readback_extraction_preserves_source_and_rejects_missing_or_duplicate_enable(self):
        original = copy.deepcopy(self.nodes)
        m.readbacks(self.nodes, self.profile)
        self.assertEqual(self.nodes, original)
        with self.assertRaisesRegex(ValueError, 'Incomplete eighteen'):
            m.readbacks(self.nodes[:-1], self.profile)
        with self.assertRaisesRegex(ValueError, 'Duplicate parameter'):
            m.readbacks(self.nodes + [copy.deepcopy(self.nodes[-1])], self.profile)

    def test_enable_must_be_numeric_one_scalar_before_and_after(self):
        for value, dimensions in ((0, None), (True, None), ('1', None), (1, [1]), (math.nan, None)):
            values = m.readbacks(self.nodes, self.profile)
            values[m.ENABLE] = dict(data=value, dimensions=dimensions)
            with self.subTest(value=value, dimensions=dimensions), self.assertRaises(ValueError):
                m.check_readbacks(values, self.profile)

    def test_registered_runtime_power_is_not_only_an_enable_check(self):
        for path in (m.normalizer.SOURCE + 'Pwr', m.normalizer.AMP + 'G', m.normalizer.SWEEP + 'StartPwr'):
            values = m.readbacks(self.nodes, self.profile)
            values[path]['data'] *= 1.001
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, 'Parameter readback value changed'):
                m.check_readbacks(values, self.profile)

    def test_decimal_source_expression_rejects_numerically_equivalent_spelling(self):
        for profile in self.plan['profiles']:
            values = m.readbacks(parameter_nodes(profile), profile)
            original = values[m.SOURCE_POWER]['data_entry']
            for changed in (original + ' ', None, float(original)):
                values[m.SOURCE_POWER]['data_entry'] = changed
                with self.subTest(profile=profile['name'], changed=changed), self.assertRaisesRegex(
                        ValueError, 'Registered Decimal source-power expression changed'):
                    m.check_readbacks(values, profile)

    def test_gain_expression_and_si_value_are_checked_independently(self):
        for index in (0, 3, 7):
            profile = self.plan['profiles'][index]
            for field, value in (('data_entry', str(profile['gain_db']) + '.0'), ('data', 1)):
                values = m.readbacks(parameter_nodes(profile), profile)
                values[m.GAIN][field] = value
                with self.subTest(profile=profile['name'], field=field), self.assertRaisesRegex(
                        ValueError, 'Registered Decimal gain expression changed|Parameter readback value changed'):
                    m.check_readbacks(values, profile)

    def test_table_si_values_and_literal_shape_are_checked(self):
        for kind in ('shape', 'value'):
            values = m.readbacks(self.nodes, self.profile)
            table = values[m.normalizer.AMP + 'AMtoPMpoints']
            if kind == 'shape':
                table['dimensions'] = [3]
            else:
                table['data'][0] = 10
            with self.subTest(mutation=kind), self.assertRaises(ValueError):
                m.check_readbacks(values, self.profile)

    def test_workflow_has_exact_prepared_and_execution_identities(self):
        before = copy.deepcopy(self.workflow)
        sources, prepared = m.check_workflow(self.workflow, self.plan)
        self.assertEqual(len(sources), 17)
        self.assertEqual(len(prepared['profiles']), 12)
        self.assertEqual(self.workflow, before)

    def test_edited_embedded_source_cannot_keep_old_sha(self):
        source = self.workflow['sources'][m.BASE + '/capture.ps1']
        source['utf8_source'] += '# changed\n'
        with self.assertRaisesRegex(ValueError, 'Embedded execution source bytes changed'):
            m.check_workflow(self.workflow, self.plan)

    def test_current_replay_code_must_match_each_frozen_module(self):
        original = Path.read_bytes
        for filename in m.REPLAY_SOURCE_FILES:
            target = SCRIPT.with_name(filename).resolve()

            def changed(path):
                raw = original(path)
                return raw + b'\n# changed replay source\n' if path.resolve() == target else raw

            with self.subTest(filename=filename), mock.patch.object(Path, 'read_bytes', changed), \
                    self.assertRaisesRegex(ValueError, 'Replay source differs from frozen workflow'):
                m.check_workflow(self.workflow, self.plan)

    def test_current_replay_source_accepts_only_line_ending_conversion(self):
        original = Path.read_bytes
        target = SCRIPT.with_name('diagnose-am-pm-datapoints-raw-axis-study.py').resolve()
        for newline in (b'\n', b'\r\n'):
            def converted(path):
                raw = original(path)
                return raw.replace(b'\r\n', b'\n').replace(b'\n', newline) if path.resolve() == target else raw

            with self.subTest(newline=newline), mock.patch.object(Path, 'read_bytes', converted):
                m.check_workflow(self.workflow, self.plan)

    def test_frozen_replay_source_collection_cannot_omit_or_add_modules(self):
        for mutation in ('remove', 'extra'):
            workflow = copy.deepcopy(self.workflow)
            path = 'scripts/reference/diagnose-am-pm-datapoints-raw-axis-study.py'
            if mutation == 'remove':
                del workflow['sources'][path]
            else:
                workflow['sources']['scripts/reference/unregistered.py'] = copy.deepcopy(workflow['sources'][path])
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, 'Frozen seventeen-source collection changed'):
                m.check_workflow(workflow, self.plan)

    def test_line_ending_bridge_proves_both_executed_and_git_bytes(self):
        path = m.BASE + '/run.py'
        original = self.workflow['sources'][path]['utf8_source']
        self.workflow['normalized_git_sources'] = {path: dict(normalization='CRLF_to_LF',
            git_sha256=sha(original.replace('\r\n', '\n').encode('utf-8')))}
        m.check_workflow(self.workflow, self.plan)
        self.workflow['normalized_git_sources'][path]['git_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'Normalized Git source identity changed'):
            m.check_workflow(self.workflow, self.plan)

    def test_missing_frozen_helper_and_unsafe_source_path_are_rejected(self):
        path = m.BASE + '/park.ps1'
        del self.workflow['sources'][path]
        with self.assertRaisesRegex(ValueError, 'Frozen seventeen-source collection changed'):
            m.check_workflow(self.workflow, self.plan)
        self.workflow = workflow_fixture(self.plan)
        self.workflow['sources']['../escape.py'] = self.workflow['sources'][m.BASE + '/run.py']
        with self.assertRaisesRegex(ValueError, 'relative'):
            m.check_workflow(self.workflow, self.plan)

    def test_prepared_manifest_hash_and_declared_parameter_edits_are_independent_guards(self):
        self.workflow['prepared_manifest_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'Prepared manifest content changed'):
            m.check_workflow(self.workflow, self.plan)
        self.workflow = workflow_fixture(self.plan)
        prepared = self.workflow['prepared_manifest']
        prepared['profiles'][0]['changes'][0]['after'] = '999'
        self.workflow['prepared_manifest_sha256'] = sha(encoded(prepared))
        with self.assertRaisesRegex(ValueError, 'registered parameters'):
            m.check_workflow(self.workflow, self.plan)

    def test_profile_order_cannot_be_changed_by_rehashing_prepared_manifest(self):
        prepared = self.workflow['prepared_manifest']
        prepared['profiles'].reverse()
        self.workflow['prepared_manifest_sha256'] = sha(encoded(prepared))
        with self.assertRaisesRegex(ValueError, 'Prepared profile collection changed'):
            m.check_workflow(self.workflow, self.plan)

    def test_same_configuration_preparation_requires_independent_paths_and_identical_bytes(self):
        for mutation in ('shared path', 'different bytes', 'false proof'):
            workflow = copy.deepcopy(self.workflow)
            prepared = workflow['prepared_manifest']
            if mutation == 'shared path':
                prepared['profiles'][11]['workspace_path'] = prepared['profiles'][0]['workspace_path']
            elif mutation == 'different bytes':
                prepared['profiles'][11]['workspace_sha256'] = '0' * 64
            else:
                prepared['same_configuration_pairs'][0]['byte_identical'] = False
            workflow['prepared_manifest_sha256'] = sha(encoded(prepared))
            with self.subTest(mutation=mutation), self.assertRaisesRegex(
                    ValueError, 'Prepared independent input path changed|Same-configuration'):
                m.check_workflow(workflow, self.plan)

    def test_prepared_decimal_expressions_and_gain_roles_survive_rehashing(self):
        for key, value in (('source_power_expression', '-20.0'), ('gain_group', 'unregistered'),
                           ('ordinary_role', 'new_prediction'), ('control_profile', 'g18-zero'),
                           ('gain_expression', '20.0')):
            workflow = copy.deepcopy(self.workflow)
            prepared = workflow['prepared_manifest']
            prepared['profiles'][0][key] = value
            workflow['prepared_manifest_sha256'] = sha(encoded(prepared))
            with self.subTest(key=key), self.assertRaisesRegex(
                    ValueError, 'Prepared Decimal .* expression changed|Prepared gain group, role, or control changed'):
                m.check_workflow(workflow, self.plan)

    def test_cross_gain_duplicate_workspace_hash_is_rejected_after_rehashing(self):
        prepared = self.workflow['prepared_manifest']
        prepared['profiles'][3]['workspace_sha256'] = prepared['profiles'][0]['workspace_sha256']
        self.workflow['prepared_manifest_sha256'] = sha(encoded(prepared))
        with self.assertRaisesRegex(ValueError, 'Different gain/source configurations reused identical workspace bytes'):
            m.check_workflow(self.workflow, self.plan)

    def test_workflow_must_preserve_gain_references_and_distinct_initial_sweeps(self):
        for field, value in (('gain_control_indices', [0]), ('repeated_sweep_indices', list(range(1, 12))),
                             ('ordinary_input_reference_indices', [0] * 12),
                             ('prepared_input_gain_expression_readback', False),
                             ('ordinary_input_raw_spectrum_validation', False)):
            workflow = copy.deepcopy(self.workflow)
            workflow['execution_gates'][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'Frozen execution gates changed'):
                m.check_workflow(workflow, self.plan)

    def test_only_same_gain_repeats_and_first_last_historical_controls_are_required(self):
        expected = {0: {'baseline-comparison.json'}, 3: set(), 7: set(),
                    11: {'baseline-comparison.json', 'repeated-sweep-comparison.json'}}
        for index in range(12):
            controls = expected.get(index, {'repeated-sweep-comparison.json'})
            self.assertEqual(set(m.comparison_files(index)), controls)
            self.assertTrue(controls <= m.artifact_files('capture', index))
            self.assertTrue(controls <= set(m.document_files('capture', index)))

    def test_recovery_accepts_only_registered_completed_copy_or_clean_workspace(self):
        status = dict(started_utc='2026-10-14T01:00:00Z', finished_utc='2026-10-14T01:00:03Z')
        directory = m.BASE + '/inputs/profiles/registered'
        cases = [('no_open_workspace', 0, False, 0, False, True),
                 ('pristine_already_open', 1, False, 0, False, True),
                 ('archived_completed_study_copy:D:/' + directory, 0, True, 0, True, True),
                 ('archived_completed_study_copy:D:/unknown', 0, True, 0, True, False),
                 ('archived_completed_study_copy:D:/../' + directory, 0, True, 0, True, False),
                 ('no_open_workspace', 1, False, 0, False, False),
                 ('no_open_workspace', False, False, 0, False, False),
                 ('no_open_workspace', 0, 0, 0, False, False),
                 ('no_open_workspace', 0, False, True, False, False),
                 ('no_open_workspace', 0, False, 1, False, False),
                 ('pristine_already_open', 1, True, 0, True, False)]
        for kind, count, flag, calls, copied, accepted in cases:
            recovery = dict(status=kind, workspace_count=count, rejected_copy_archived=flag,
                            analysis_calls=calls, observed_utc='2026-10-14T01:00:01Z')
            data = encoded(recovery)
            artifacts = {'recovery-before-open.json': sha(data)}
            if copied:
                artifacts[m.RECOVERY_COPY] = '0' * 64
            row = dict(documents={'recovery-before-open.json': data.decode('utf-8')},
                       validation=dict(artifact_sha256=artifacts),
                       workspace_observation=dict(observed_utc='2026-10-14T01:00:02Z'))
            with self.subTest(kind=kind, count=count, flag=flag, calls=calls):
                if accepted:
                    m.check_recovery(row, status, [directory])
                else:
                    with self.assertRaises(ValueError):
                        m.check_recovery(row, status, [directory])

    def test_historical_restoration_snapshot_requires_its_own_byte_identity(self):
        historical = self.workflow['historical_restoration']
        historical['parameter_snapshot'][next(iter(historical['parameter_snapshot']))][0]['data'] = 1
        with self.assertRaisesRegex(ValueError, 'Historical ordered parameter snapshot changed'):
            m.check_workflow(self.workflow, self.plan)

    def test_exact_embedded_document_bytes_are_verified_before_json_loading(self):
        text = '{"synthetic":true,"analysis_done":1}\r\n'
        row = dict(documents={'completion.json': text},
                   validation=dict(artifact_sha256={'completion.json': sha(text.encode('utf-8'))}))
        self.assertEqual(m.document(row, 'completion.json'), dict(synthetic=True, analysis_done=1))
        row['documents']['completion.json'] = text.replace('\r\n', '\n')
        with self.assertRaisesRegex(ValueError, 'Embedded record differs'):
            m.document(row, 'completion.json')


class CommittedAllocationAcquisitionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for relative in FIXTURES:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, path)
        self.plan_path = self.root / FIXTURES[0]
        self.manifest = m.load(self.root / MANIFEST)
        self.observations = m.load(self.root / OBSERVATIONS)

    def verify(self):
        return m.verify_committed(self.root, self.plan_path, self.root / MANIFEST, self.root / OBSERVATIONS)

    def save(self, manifest=None, observations=None):
        manifest = self.manifest if manifest is None else manifest
        observations = copy.deepcopy(self.observations if observations is None else observations)
        data = encoded(manifest)
        (self.root / MANIFEST).write_bytes(data)
        observations['capture_manifest_sha256'] = sha(data)
        (self.root / OBSERVATIONS).write_bytes(encoded(observations))

    def refresh_document(self, row, filename, value):
        data = encoded(value)
        row['documents'][filename] = data.decode('utf-8')
        row['validation']['artifact_sha256'][filename] = sha(data)
        row['validated_sha256'] = sha(encoded(row['validation']))

    def test_complete_committed_chain_verifies_without_raw_files(self):
        before = {name: (self.root / name).read_bytes() for name in FIXTURES}
        result = self.verify()
        self.assertIs(result['committed_archive_verified'], True)
        self.assertIs(result['raw_source_bytes_verified'], False)
        self.assertEqual(result['capture_count'], 12)
        self.assertEqual(result['planned_sweep_points'], 732)
        self.assertEqual(result['new_prediction_checks'], 8)
        self.assertEqual(result['historical_reproductions'], 4)
        self.assertEqual(result['divergent_prediction_checks'], 6)
        self.assertEqual(result['common_nonzero_prediction_checks'], 2)
        self.assertEqual(result['same_nominal_output_pairs'], 6)
        self.assertEqual(result['same_nominal_input_pairs'], 2)
        self.assertEqual(result['same_configuration_pairs'], 1)
        self.assertEqual(result['invocation_count'], 15)
        self.assertEqual(result['raw_input_nodes_per_capture'], 7)
        self.assertEqual(result['raw_input_carriers_per_capture'], 2)
        self.assertEqual(result['baseline_gates_passed'], 2)
        self.assertEqual(result['parameter_readbacks_per_capture'], 18)
        self.assertEqual(result['repeated_sweep_gates_passed'], 9)
        self.assertEqual(result['restored_parameter_occurrences'], 247)
        self.assertFalse(result['eligible_for_full_model_compatibility'])
        self.assertFalse((self.root / 'build-reference').exists())
        self.assertEqual({name: (self.root / name).read_bytes() for name in FIXTURES}, before)

    def test_sample_collection_preserves_all_ordinary_sweep_and_raw_carrier_points(self):
        plan = m.load(self.plan_path)
        samples = self.observations['samples']
        self.assertEqual([sample['profile'] for sample in samples], plan['profiles'])
        self.assertEqual([len(sample['sweep']) for sample in samples], [61] * 12)
        self.assertEqual(sum(len(sample['ordinary']['raw_carrier']) for sample in samples), 24)
        self.assertEqual(sum(len(sample['ordinary']['raw_input_carrier']) for sample in samples), 24)
        self.assertEqual(sum(len(sample['ordinary']['raw_input_spectrum_nodes']) for sample in samples), 84)
        self.assertEqual(len({row['directory'] for row in self.manifest['captures']}), 12)
        self.assertEqual(sum(len(sample['additional_sweep_points']) for sample in samples),
                         self.manifest['extra_sweep_points'])

    def test_workflow_and_historical_baseline_files_are_required_and_byte_bound(self):
        for relative in (FIXTURES[1], FIXTURES[-1]):
            path = self.root / relative
            original = path.read_bytes()
            with self.subTest(path=relative, mutation='missing'):
                path.unlink()
                with self.assertRaises((ValueError, FileNotFoundError)):
                    self.verify()
                path.write_bytes(original)
            with self.subTest(path=relative, mutation='same JSON different bytes'):
                path.write_bytes(original + b'\n')
                with self.assertRaises(ValueError):
                    self.verify()
                path.write_bytes(original)

    def test_before_and_after_enable_readbacks_cannot_disappear_or_change(self):
        for stage in ('parameters_before', 'parameters_after'):
            for mutation in ('remove', 'disable', 'array shape'):
                manifest = copy.deepcopy(self.manifest)
                readbacks = manifest['captures'][0][stage]
                if mutation == 'remove':
                    del readbacks[m.ENABLE]
                elif mutation == 'disable':
                    readbacks[m.ENABLE]['data'] = 0
                else:
                    readbacks[m.ENABLE]['dimensions'] = [1]
                with self.subTest(stage=stage, mutation=mutation):
                    self.save(manifest)
                    with self.assertRaises(ValueError):
                        self.verify()

    def test_before_and_after_decimal_source_expression_is_mandatory(self):
        for stage in ('parameters_before', 'parameters_after'):
            manifest = copy.deepcopy(self.manifest)
            manifest['captures'][11][stage][m.SOURCE_POWER]['data_entry'] = '-20.0'
            self.save(manifest)
            with self.subTest(stage=stage), self.assertRaisesRegex(
                    ValueError, 'Registered Decimal source-power expression changed'):
                self.verify()

    def test_before_and_after_gain_expression_is_mandatory_for_each_gain_group(self):
        for index in (0, 3, 7):
            for stage in ('parameters_before', 'parameters_after'):
                manifest = copy.deepcopy(self.manifest)
                manifest['captures'][index][stage][m.GAIN]['data_entry'] += '.0'
                self.save(manifest)
                with self.subTest(index=index, stage=stage), self.assertRaisesRegex(
                        ValueError, 'Registered Decimal gain expression changed'):
                    self.verify()

    def test_collector_routes_every_repeat_to_its_registered_gain_control(self):
        original = m.scorer_module()
        observed = []

        class ScorerProxy:
            def __getattr__(self, name):
                return getattr(original, name)

            def compare_repeated_sweep(self, plan, sample, control):
                observed.append((sample['profile']['name'], control['profile']['name']))
                return original.compare_repeated_sweep(plan, sample, control)

        with mock.patch.object(m, 'scorer_module', return_value=ScorerProxy()):
            self.verify()
        plan = m.load(self.plan_path)
        self.assertEqual(observed, [(plan['profiles'][index]['name'], plan['profiles'][index]['control_profile'])
                                    for index in plan['repeated_sweep_indices']])

    def test_first_g18_and_g22_sweeps_cannot_be_relabelled_as_historical_or_repeated_controls(self):
        for index in (3, 7):
            for filename in ('baseline-comparison.json', 'repeated-sweep-comparison.json'):
                manifest = copy.deepcopy(self.manifest)
                row = manifest['captures'][index]
                donor = manifest['captures'][0 if filename == 'baseline-comparison.json' else 1]
                row['documents'][filename] = donor['documents'][filename]
                row['validation']['artifact_sha256'][filename] = donor['validation']['artifact_sha256'][filename]
                row['validated_sha256'] = sha(encoded(row['validation']))
                self.save(manifest)
                with self.subTest(index=index, filename=filename), self.assertRaisesRegex(
                        ValueError, 'Incomplete invocation artifact set'):
                    self.verify()

    def test_raw_input_nodes_and_carriers_are_validated_before_sample_hashes(self):
        for mutation in ('power', 'carrier type', 'node frequency'):
            observations = copy.deepcopy(self.observations)
            ordinary = observations['samples'][0]['ordinary']
            if mutation == 'power':
                ordinary['raw_input_w'] = math.nextafter(ordinary['raw_input_w'], math.inf)
            elif mutation == 'carrier type':
                ordinary['raw_input_carrier'][0]['signal_id'] = str(ordinary['raw_input_carrier'][0]['signal_id'])
            else:
                frequency = next(node for node in ordinary['raw_input_spectrum_nodes'] if node['path'].endswith('/F1'))
                frequency['data'][ordinary['raw_input_carrier'][0]['raw_index']] += 1
            self.save(observations=observations)
            with self.subTest(mutation=mutation), self.assertRaisesRegex(
                    ValueError, 'Retained raw input|Unexpected raw input carrier support'):
                self.verify()

    def test_raw_input_metadata_remains_bound_to_original_normalized_artifact(self):
        manifest, observations = copy.deepcopy(self.manifest), copy.deepcopy(self.observations)
        sample = observations['samples'][0]
        sample['ordinary']['raw_input_spectrum_nodes'][0]['variables'].append('modified metadata')
        normalized = {key: value for key, value in sample.items() if key != 'capture_source'}
        manifest['captures'][0]['sample_sha256'] = m.canonical(normalized)
        sample['capture_source']['sample_sha256'] = m.canonical(normalized)
        self.save(manifest, observations)
        with self.assertRaisesRegex(ValueError, 'Normalized sample differs from recorded raw artifact'):
            self.verify()

    def test_zero_open_workspace_precondition_and_capture_names_are_mandatory(self):
        for key, value in (('pre_open_workspace_count', 1), ('pre_open_workspace_count', False),
                           ('workspace_names', []), ('workspace_names', ['RFModel_AMPM_A01', 'Blank'])):
            manifest = copy.deepcopy(self.manifest)
            manifest['captures'][11]['before_workspace_observation'][key] = value
            self.save(manifest)
            with self.subTest(key=key, value=value), self.assertRaisesRegex(
                    ValueError, 'Capture did not begin with zero open workspaces|Capture workspace identity changed'):
                self.verify()

    def test_final_historical_and_same_gain_repeat_gates_cannot_be_removed_or_falsified(self):
        for filename in ('baseline-comparison.json', 'repeated-sweep-comparison.json'):
            for mutation in ('remove', 'false verdict'):
                manifest = copy.deepcopy(self.manifest)
                row = manifest['captures'][11]
                if mutation == 'remove':
                    del row['documents'][filename]
                    del row['validation']['artifact_sha256'][filename]
                    row['validated_sha256'] = sha(encoded(row['validation']))
                else:
                    value = m.document(row, filename)
                    value['all_comparisons_matched'] = False
                    self.refresh_document(row, filename, value)
                self.save(manifest)
                with self.subTest(filename=filename, mutation=mutation), self.assertRaisesRegex(
                        ValueError, 'Incomplete invocation artifact set|Replay .*value changed'):
                    self.verify()

    def test_application_identity_and_completion_cannot_change_after_rehashing(self):
        for filename, key, value in (('application.json', 'pid', True),
                                     ('application.json', 'started_utc', '2026-10-14T00:00:00Z'),
                                     ('application.json', 'file_version', 'unregistered'),
                                     ('completion.json', 'analysis_done', True),
                                     ('completion.json', 'analysis_done', '1'),
                                     ('completion.json', 'manager_errors', 'analysis warning')):
            manifest = copy.deepcopy(self.manifest)
            row = manifest['captures'][11]
            document = m.document(row, filename)
            document[key] = value
            self.refresh_document(row, filename, document)
            self.save(manifest)
            with self.subTest(filename=filename, key=key), self.assertRaisesRegex(
                    ValueError, 'Vendor application identity changed|Analysis incomplete or diagnostic'):
                self.verify()

    def test_recovery_document_and_chronology_remain_required(self):
        for mutation in ('missing', 'late'):
            manifest = copy.deepcopy(self.manifest)
            row = manifest['restore']
            filename = 'recovery-before-open.json'
            if mutation == 'missing':
                del row['documents'][filename]
            else:
                value = m.document(row, filename)
                value['observed_utc'] = '2099-01-01T00:00:00Z'
                self.refresh_document(row, filename, value)
            self.save(manifest)
            with self.subTest(mutation=mutation), self.assertRaisesRegex(
                    ValueError, 'Embedded document collection changed|Restoration recovery chronology changed'):
                self.verify()

    def test_seventeen_parameter_intent_is_rejected_after_rehashing_all_envelopes(self):
        manifest = copy.deepcopy(self.manifest)
        row = manifest['captures'][0]
        intent = json.loads(row['documents']['run-intent.json'].lstrip('\ufeff'))
        intent['validated_parameter_count'] = 17
        self.refresh_document(row, 'run-intent.json', intent)
        self.save(manifest)
        with self.assertRaisesRegex(ValueError, 'eighteen-parameter analysis intent'):
            self.verify()

    def test_status_chronology_still_applies_after_rehashing_status_and_validation(self):
        manifest = copy.deepcopy(self.manifest)
        row = manifest['captures'][1]
        status = json.loads(row['documents']['status.json'].lstrip('\ufeff'))
        status['started_utc'] = self.observations['samples'][0]['run_started_utc']
        self.refresh_document(row, 'status.json', status)
        self.save(manifest)
        with self.assertRaisesRegex(ValueError, 'Next call preceded predecessor validation or completion'):
            self.verify()

    def test_source_snapshot_remains_bound_to_frozen_execution_source(self):
        manifest = copy.deepcopy(self.manifest)
        old_hash = manifest['captures'][0]['validation']['artifact_sha256']['source-snapshot.json']
        snapshot = json.loads(manifest['source_snapshots'][old_hash]['utf8_json'].lstrip('\ufeff'))
        paths = [path for path in snapshot if path.replace('\\', '/') == m.BASE + '/capture.ps1']
        self.assertEqual(len(paths), 1)
        source = snapshot[paths[0]]
        source['utf8_source'] += '\n# edited execution source\n'
        source['sha256'] = sha(source['utf8_source'].encode('utf-8'))
        data = encoded(snapshot)
        new_hash = sha(data)
        del manifest['source_snapshots'][old_hash]
        manifest['source_snapshots'][new_hash] = {'utf8_json': data.decode('utf-8')}
        for row in [manifest['preflight'], manifest['park']] + manifest['captures'] + [manifest['restore']]:
            artifacts = row['validation']['artifact_sha256']
            if artifacts['source-snapshot.json'] == old_hash:
                artifacts['source-snapshot.json'] = new_hash
                row['validated_sha256'] = sha(encoded(row['validation']))
        self.save(manifest)
        with self.assertRaisesRegex(ValueError, 'Executed source snapshot differs from frozen workflow'):
            self.verify()

    def test_baseline_and_repeated_sweep_records_must_match_recomputation(self):
        for index, filename in ((0, 'baseline-comparison.json'), (1, 'repeated-sweep-comparison.json')):
            manifest = copy.deepcopy(self.manifest)
            row = manifest['captures'][index]
            row['validation']['artifact_sha256'][filename] = '0' * 64
            row['validated_sha256'] = sha(encoded(row['validation']))
            with self.subTest(filename=filename):
                self.save(manifest)
                with self.assertRaisesRegex(ValueError, 'Embedded record differs from retained raw artifact'):
                    self.verify()

    def test_partial_score_and_controls_retain_exact_original_document_bytes(self):
        for index, filename in ((0, 'partial-score.json'), (0, 'baseline-comparison.json'),
                                (1, 'repeated-sweep-comparison.json')):
            manifest = copy.deepcopy(self.manifest)
            row = manifest['captures'][index]
            row['documents'][filename] += '\n'
            with self.subTest(filename=filename):
                self.save(manifest)
                with self.assertRaisesRegex(ValueError, 'Embedded record differs from retained raw artifact'):
                    self.verify()

    def test_rehashed_derived_document_rejects_material_numeric_change(self):
        for index, filename in ((0, 'partial-score.json'), (0, 'baseline-comparison.json'),
                                (1, 'repeated-sweep-comparison.json')):
            manifest = copy.deepcopy(self.manifest)
            row = manifest['captures'][index]
            value = m.document(row, filename)
            if filename == 'partial-score.json':
                value['historical_reproductions']['rows'][0]['observed_increment_deg'] += 1e-9
            else:
                value['rows'][0]['phases']['increment']['new_deg'] += 1e-9
            self.refresh_document(row, filename, value)
            with self.subTest(filename=filename):
                self.save(manifest)
                with self.assertRaisesRegex(ValueError, 'Replay numeric value changed'):
                    self.verify()

    def test_rehashed_partial_score_rejects_verdict_or_raw_value_changes(self):
        for mutation in ('verdict', 'raw float', 'tolerance'):
            manifest = copy.deepcopy(self.manifest)
            row = manifest['captures'][0]
            value = m.document(row, 'partial-score.json')
            control = value['historical_reproductions']['rows'][0]
            if mutation == 'verdict':
                control['legacy_hypotheses']['raw_continuous']['passed'] = not control['legacy_hypotheses']['raw_continuous']['passed']
            elif mutation == 'raw float':
                control['input_power_w'] = math.nextafter(control['input_power_w'], math.inf)
            else:
                value['phase_absolute_tolerance_degrees'] = math.nextafter(value['phase_absolute_tolerance_degrees'], math.inf)
            self.refresh_document(row, 'partial-score.json', value)
            self.save(manifest)
            with self.subTest(mutation=mutation), self.assertRaisesRegex(ValueError, 'Replay .*value changed'):
                self.verify()

    def test_canonical_rehash_cannot_replace_normalized_artifact_bytes(self):
        manifest, observations = copy.deepcopy(self.manifest), copy.deepcopy(self.observations)
        sample = observations['samples'][-1]
        sample['archive_integrity_test_marker'] = True
        normalized = {key: value for key, value in sample.items() if key != 'capture_source'}
        digest = sha(json.dumps(normalized, sort_keys=True, separators=(',', ':'),
                                allow_nan=False).encode('utf-8'))
        manifest['captures'][-1]['sample_sha256'] = digest
        sample['capture_source']['sample_sha256'] = digest
        self.save(manifest, observations)
        with self.assertRaisesRegex(ValueError, 'Normalized sample differs from recorded raw artifact'):
            self.verify()

    def test_ordered_restoration_is_tied_to_historical_state(self):
        manifest = copy.deepcopy(self.manifest)
        key = 'RFModel_AMPM_A01/DataPoints/System2/Path1/DigNumAvg'
        manifest['parameter_snapshot'][key][1]['data'] = 1
        manifest['parameter_snapshot_sha256'] = sha(encoded(manifest['parameter_snapshot']))
        self.save(manifest)
        with self.assertRaisesRegex(ValueError, 'Restoration differs from frozen historical parameter state'):
            self.verify()

    def test_preflight_and_restored_workspace_names_cannot_change_after_rehashing(self):
        for mode in ('preflight', 'restore'):
            for names in ([], ['Blank'], ['RFModel_AMPM_A01', 'Blank']):
                manifest = copy.deepcopy(self.manifest)
                manifest[mode]['workspace_observation']['workspace_names'] = names
                self.save(manifest)
                with self.subTest(mode=mode, names=names), \
                        self.assertRaisesRegex(ValueError, 'Workspace control identity changed'):
                    self.verify()

    def test_observation_order_and_completeness_are_mandatory(self):
        for mutation in ('reverse', 'remove'):
            observations = copy.deepcopy(self.observations)
            if mutation == 'reverse':
                observations['samples'].reverse()
            else:
                observations['samples'].pop()
            with self.subTest(mutation=mutation):
                self.save(observations=observations)
                with self.assertRaises(ValueError):
                    self.verify()

    def test_call_count_and_raw_access_scope_cannot_be_inflated(self):
        for field, value in (('analysis_calls', 13), ('analysis_calls', True),
                             ('reserved_analysis_calls_used', 1), ('new_prediction_checks', 11),
                             ('planned_sweep_points', 682), ('vendor_workspace_bytes_embedded', True),
                             ('raw_source_bytes_verified_at_collection', False)):
            manifest = copy.deepcopy(self.manifest)
            manifest[field] = value
            with self.subTest(field=field, value=value):
                self.save(manifest)
                with self.assertRaisesRegex(ValueError, 'Acquisition counts changed|Archive scope changed'):
                    self.verify()

    def test_full_raw_verification_cannot_be_substituted_by_committed_hashes(self):
        with self.assertRaises((ValueError, FileNotFoundError)):
            m.verify_archive(self.root, self.plan_path, self.root / MANIFEST, self.root / OBSERVATIONS)


class ReplayComparisonTests(unittest.TestCase):
    def test_only_named_computed_floats_accept_small_libm_differences(self):
        for field in m.REPLAY_COMPUTED_FLOAT_FIELDS:
            with self.subTest(field=field):
                m.compare_replay({'nested': [{field: 37.0}]}, {'nested': [{field: 37.0 + 1e-13}]})
                with self.assertRaisesRegex(ValueError, 'Replay numeric value changed'):
                    m.compare_replay({field: 37.0}, {field: 37.0 + 1e-9})

    def test_replay_allowance_is_absolute_even_for_large_computed_values(self):
        with self.assertRaisesRegex(ValueError, 'Replay numeric value changed'):
            m.compare_replay({'circular_error_deg': 1e6}, {'circular_error_deg': 1e6 + 1e-8})

    def test_registered_ratio_has_separate_tight_relative_allowance(self):
        for field in m.REPLAY_RELATIVE_FLOAT_FIELDS:
            with self.subTest(field=field):
                m.compare_replay({field: 1.0}, {field: math.nextafter(1.0, math.inf)})
                with self.assertRaisesRegex(ValueError, 'Replay numeric value changed'):
                    m.compare_replay({field: 1.0}, {field: 1.0 + 1e-13})

    def test_raw_observations_and_scientific_tolerances_remain_exact(self):
        for field in ('input_power_w', 'output_power_w', 'raw_output_w', 'input_phase_rad',
                      'output_phase_rad', 'new_w', 'reference_w', 'nominal_output_dbm',
                      'relative_tolerance', 'phase_absolute_tolerance_degrees', 'measured_input_ratio',
                      'raw_input_w', 'raw_input_power_w', 'left_w', 'right_w', 'signed_difference_w',
                      'absolute_difference_w', 'raw_gain_linear', 'raw_input_ratio', 'ratio'):
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'Replay numeric value changed'):
                m.compare_replay({field: 1.0}, {field: math.nextafter(1.0, math.inf)})

    def test_types_shapes_counts_and_verdicts_remain_exact(self):
        for before, after in (({'passed': True}, {'passed': 1}), ({'passed': True}, {'passed': False}),
                              ({'count': 1}, {'count': 1.0}), ({'count': 1}, {'count': 2}),
                              ({'circular_error_deg': 0}, {'circular_error_deg': 0.0}),
                              ({'value': None}, {}), ([1], [1, 2]), ('same', 'different')):
            with self.subTest(before=before, after=after), self.assertRaises(ValueError):
                m.compare_replay(before, after)

    def test_nonfinite_values_are_rejected_even_when_both_match(self):
        for value in (math.nan, math.inf, -math.inf):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'Non-finite replay value'):
                m.compare_replay({'circular_error_deg': value}, {'circular_error_deg': value})


if __name__ == '__main__':
    unittest.main()

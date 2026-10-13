"""Allocation-study preparation tests using synthetic XML, without vendor files."""

import copy
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET


def module(filename):
    spec = importlib.util.spec_from_file_location(filename, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


m = module('prepare-am-pm-datapoints-allocation-study.py')
fixture = module('test_prepare_am_pm_datapoints_raw_axis_study.py').fixture
PLAN = Path(__file__).resolve().parents[2] / 'validation/am-pm-datapoints-allocation-plan-20261014.json'
SOURCE_POWER = 'DataPoints/Sch2/PartList/Source/ParamSet/Pwr/DataEntry'
GAIN = 'DataPoints/Sch2/PartList/RFAmp/ParamSet/G/DataEntry'
EXPRESSIONS = ('-20', '-20.000001', '-19.999999', '-18', '-18.000001', '-17.999999',
               '-20', '-22', '-21.999999', '-22.000001', '-20', '-20')


class AllocationPreparationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name) / 'root'
        self.root.mkdir()
        self.raw = fixture()
        self.source = self.root / m.SOURCE_PATH
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(self.raw)
        self.plan = json.loads(PLAN.read_bytes())
        self.plan['source_workspace_sha256'] = m.digest(self.raw)
        self.plan_path = self.root / 'plan.json'
        self.output = self.root / 'inputs'
        self.source_patch = mock.patch.object(m, 'SOURCE_SHA256', m.digest(self.raw))
        self.source_patch.start()
        self.addCleanup(self.source_patch.stop)

    def prepare(self, plan=None, output=None):
        self.plan_path.write_text(json.dumps(self.plan if plan is None else plan), encoding='utf-8')
        with mock.patch.object(m, 'PLAN_SHA256', m.digest(self.plan_path.read_bytes())):
            return m.prepare(self.root, self.plan_path, self.output if output is None else output)

    def test_registered_plan_bytes_and_reused_preparer_identity(self):
        self.assertEqual(m.digest(PLAN.read_bytes()), m.PLAN_SHA256)
        self.assertIs(m.prepare_document, m.previous.prepare_document)
        self.assertIs(m.planned_fields, m.previous.planned_fields)
        self.assertIs(m.source_expression, m.previous.source_expression)
        self.assertIs(m.gain_expression, m.previous.expression)
        self.assertEqual(m.SOURCE_PATH, m.previous.SOURCE_PATH)
        self.assertEqual(json.loads(PLAN.read_bytes())['source_workspace_sha256'], m.previous.SOURCE_SHA256)

    def test_gain_and_decimal_source_expressions_are_exact_in_xml(self):
        for profile, expected in zip(self.plan['profiles'], EXPRESSIONS):
            with self.subTest(profile=profile['name']):
                self.assertEqual(m.source_expression(profile['ordinary_output_target_dbm'], profile['gain_db']), expected)
                self.assertEqual(m.planned_fields(profile)[SOURCE_POWER], expected)
                data, changes = m.prepare_document(self.raw, profile)
                tree = ET.fromstring(data)
                self.assertEqual(m.previous.element(tree, SOURCE_POWER).text, expected)
                self.assertEqual(m.previous.element(tree, GAIN).text, str(profile['gain_db']))
                self.assertEqual(len(changes), 17)

    def test_twelve_independent_inputs_preserve_only_registered_same_configuration_pair(self):
        result = self.prepare()
        self.assertEqual(result['independent_input_directories'], 12)
        self.assertEqual(len(result['profiles']), 12)
        self.assertEqual(len(result['same_configuration_pairs']), 1)
        self.assertNotIn('same_target_pairs', result)
        paths = [self.root / row['workspace_path'] for row in result['profiles']]
        self.assertEqual(len({path.parent for path in paths}), 12)
        self.assertEqual(len({path.read_bytes() for path in paths}), 11)
        for row, profile, path, source in zip(result['profiles'], self.plan['profiles'], paths, EXPRESSIONS):
            self.assertEqual(row['name'], profile['name'])
            self.assertEqual(row['workspace_sha256'], m.digest(path.read_bytes()))
            self.assertEqual(row['source_power_expression'], source)
            self.assertEqual(row['gain_expression'], str(profile['gain_db']))
            self.assertEqual(len(row['changes']), 17)
            self.assertEqual({change['path']: change['after'] for change in row['changes']}, m.planned_fields(profile))
            for field in ('ordinary_role', 'gain_group', 'comparison_role', 'control_profile'):
                self.assertEqual(row[field], profile[field])
        pair = result['same_configuration_pairs'][0]
        self.assertEqual(pair['profiles'], ['g20-start-zero', 'g20-end-zero'])
        self.assertEqual(pair['gain_db'], 20)
        self.assertEqual(pair['ordinary_output_target_dbm'], 0)
        self.assertIs(pair['byte_identical'], True)
        self.assertEqual(paths[0].read_bytes(), paths[-1].read_bytes())
        self.assertEqual(result['profiles'][0]['changes'], result['profiles'][-1]['changes'])
        self.assertEqual(pair['workspace_sha256'], m.digest(paths[0].read_bytes()))
        self.assertEqual(json.loads((self.output / 'prepared-workspaces.json').read_bytes()), result)
        self.assertEqual(self.source.read_bytes(), self.raw)

    def test_equal_nominal_output_or_input_does_not_share_configuration_bytes(self):
        result = self.prepare()
        rows = result['profiles']
        for indices in ((0, 3, 7), (1, 4, 9), (2, 5, 8), (0, 6, 10)):
            with self.subTest(indices=indices):
                self.assertEqual(len({rows[index]['workspace_sha256'] for index in indices}), 3)
        self.assertEqual([rows[index]['source_power_expression'] for index in (0, 6, 10)], ['-20'] * 3)
        self.assertEqual([rows[index]['gain_expression'] for index in (0, 6, 10)], ['20', '18', '22'])

    def test_all_twelve_inverse_semantic_audits_and_unedited_xml_bytes(self):
        mask = lambda raw: re.sub(rb'<(?:DataEntry|D)(?:\s*/>|>[^<]*</(?:DataEntry|D)>)', b'<edited/>', raw)
        for profile in self.plan['profiles']:
            data, changes = m.prepare_document(self.raw, profile)
            inverse = ET.fromstring(data)
            for change in changes:
                m.previous.element(inverse, change['path']).text = change['before']
            with self.subTest(profile=profile['name']):
                self.assertEqual(m.previous.signature(inverse), m.previous.signature(ET.fromstring(self.raw)))
                self.assertEqual(mask(data), mask(self.raw))

    def test_output_escape_and_existing_destination_are_rejected_without_writes(self):
        for bad in (self.root, self.root.parent / 'escape', self.root / '..' / 'escaped'):
            with self.subTest(path=bad), self.assertRaisesRegex(ValueError, 'remain inside root'):
                self.prepare(output=bad)
        self.prepare()
        before = {path.relative_to(self.output): path.read_bytes() for path in self.output.rglob('*') if path.is_file()}
        with self.assertRaisesRegex(ValueError, 'output already exists'):
            self.prepare()
        self.assertEqual(before, {path.relative_to(self.output): path.read_bytes() for path in self.output.rglob('*') if path.is_file()})

    def test_changed_plan_bytes_and_source_bytes_are_rejected(self):
        self.plan_path.write_text(json.dumps(self.plan), encoding='utf-8')
        with mock.patch.object(m, 'PLAN_SHA256', m.digest(self.plan_path.read_bytes())):
            self.plan_path.write_bytes(self.plan_path.read_bytes() + b'\n')
            with self.assertRaisesRegex(ValueError, 'Registered plan hash changed'):
                m.prepare(self.root, self.plan_path, self.output)
        self.assertFalse(self.output.exists())
        self.source.write_bytes(self.raw + b' ')
        with self.assertRaisesRegex(ValueError, 'Source workspace hash changed'):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_rehashed_plan_cannot_change_source_order_role_group_control_or_targets(self):
        changes = [('source_workspace_sha256', '0' * 64)]
        for field, value in changes:
            plan = copy.deepcopy(self.plan)
            plan[field] = value
            with self.assertRaises(ValueError):
                self.prepare(plan)
        for field, value in (('name', 'g18-zero'), ('ordinary_role', 'historical_reproduction'),
                             ('gain_group', 'g20'), ('comparison_role', 'same_nominal_input'),
                             ('control_profile', 'g20-start-zero'), ('gain_db', 20),
                             ('ordinary_output_target_dbm', False)):
            plan = copy.deepcopy(self.plan)
            plan['profiles'][4][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.prepare(plan)
            self.assertFalse(self.output.exists())
        for mutation in ('order', 'count', 'extra field', 'budget'):
            plan = copy.deepcopy(self.plan)
            if mutation == 'order':
                plan['profiles'][8:10] = reversed(plan['profiles'][8:10])
            elif mutation == 'count':
                plan['profiles'].pop()
            elif mutation == 'extra field':
                plan['profiles'][-1]['unregistered'] = True
            else:
                plan['analysis_budget']['reserved_setup_recovery'] = False
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.prepare(plan)
            self.assertFalse(self.output.exists())

    def test_late_profile_physics_failure_precedes_all_destination_writes(self):
        for field, value in (('source_phase_deg', 0), ('gain_db', True), ('sweep_step_db', 0),
                             ('power_dbm', [0, 10, 20]), ('phase_deg', [10, 40, -21]),
                             ('sweep_input_stop_dbm', False), ('expected_points', 61.0)):
            plan = copy.deepcopy(self.plan)
            plan['profiles'][-1][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.prepare(plan)
            self.assertFalse(self.output.exists())

    def test_same_configuration_byte_mismatch_is_rejected_before_writing(self):
        original = m.prepare_document

        def divergent(raw, profile):
            data, changes = original(raw, profile)
            return (data + b' ' if profile['name'] == 'g20-end-zero' else data), changes

        with mock.patch.object(m, 'prepare_document', divergent), self.assertRaisesRegex(
                ValueError, 'Same-configuration prepared workspace bytes differ'):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_cross_gain_reused_bytes_are_rejected_before_writing(self):
        original = m.prepare_document

        def duplicate(raw, profile):
            data, changes = original(raw, profile)
            if profile['name'] == 'g18-zero':
                data = original(raw, self.plan['profiles'][0])[0]
            return data, changes

        with mock.patch.object(m, 'prepare_document', duplicate), self.assertRaisesRegex(
                ValueError, 'Different configurations reused identical workspace bytes'):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_xml_failure_and_incomplete_edit_ledger_precede_destination_writes(self):
        with mock.patch.object(m, 'prepare_document', return_value=(b'<synthetic/>', [])), \
                self.assertRaisesRegex(ValueError, 'seventeen registered XML edits'):
            self.prepare()
        self.assertFalse(self.output.exists())
        broken = self.raw.replace(b'<DataEntry>old-expression</DataEntry>',
                                  b'<DataEntry>x</DataEntry><DataEntry>y</DataEntry>', 1)
        self.source.write_bytes(broken)
        self.plan['source_workspace_sha256'] = m.digest(broken)
        with mock.patch.object(m, 'SOURCE_SHA256', m.digest(broken)), self.assertRaises(ValueError):
            self.prepare()
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()

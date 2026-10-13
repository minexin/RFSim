"""Order-study preparation tests using synthetic XML, without vendor files."""

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


m = module('prepare-am-pm-datapoints-order-study.py')
fixture = module('test_prepare_am_pm_datapoints_raw_axis_study.py').fixture
PLAN = Path(__file__).resolve().parents[2] / 'validation/am-pm-datapoints-order-plan-20261014.json'
SOURCE_POWER = 'DataPoints/Sch2/PartList/Source/ParamSet/Pwr/DataEntry'
EXPRESSIONS = {'zero': '-20', 'positive-epsilon': '-19.999999', 'negative-epsilon': '-20.000001',
               'positive-delta': '-19.99999', 'negative-delta': '-20.00001'}


class OrderPreparationTests(unittest.TestCase):
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
        self.assertEqual(m.SOURCE_PATH, m.previous.SOURCE_PATH)

    def test_decimal_epsilon_and_delta_source_expressions_are_exact(self):
        for profile in self.plan['profiles']:
            expected = EXPRESSIONS[profile['name'][2:]]
            with self.subTest(profile=profile['name']):
                self.assertEqual(m.source_expression(profile['ordinary_output_target_dbm'], 20), expected)
                self.assertEqual(m.planned_fields(profile)[SOURCE_POWER], expected)
                data, changes = m.prepare_document(self.raw, profile)
                self.assertEqual(m.previous.element(ET.fromstring(data), SOURCE_POWER).text, expected)
                self.assertEqual(len(changes), 17)

    def test_ten_independent_inputs_preserve_exact_ab_bytes_and_registered_roles(self):
        result = self.prepare()
        self.assertEqual(result['independent_input_directories'], 10)
        self.assertEqual(len(result['profiles']), 10)
        self.assertEqual(len(result['same_target_pairs']), 5)
        paths = [self.root / row['workspace_path'] for row in result['profiles']]
        self.assertEqual(len({path.parent for path in paths}), 10)
        self.assertEqual(len({path.read_bytes() for path in paths}), 5)
        by_name = {row['name']: row for row in result['profiles']}
        for row, profile, path in zip(result['profiles'], self.plan['profiles'], paths):
            self.assertEqual(row['name'], profile['name'])
            self.assertEqual(row['workspace_sha256'], m.digest(path.read_bytes()))
            self.assertEqual(row['source_power_expression'], EXPRESSIONS[profile['name'][2:]])
            self.assertEqual(len(row['changes']), 17)
            self.assertEqual({change['path']: change['after'] for change in row['changes']}, m.planned_fields(profile))
            for field in ('block', 'block_position', 'ordinary_role'):
                self.assertEqual(row[field], profile[field])
        for pair in result['same_target_pairs']:
            left, right = [by_name[name] for name in pair['profiles']]
            self.assertNotEqual(left['workspace_path'], right['workspace_path'])
            self.assertEqual((self.root / left['workspace_path']).read_bytes(), (self.root / right['workspace_path']).read_bytes())
            self.assertEqual(left['changes'], right['changes'])
            self.assertEqual(left['workspace_sha256'], right['workspace_sha256'])
            self.assertEqual(pair['workspace_sha256'], left['workspace_sha256'])
            self.assertIs(pair['byte_identical'], True)
        self.assertEqual(json.loads((self.output / 'prepared-workspaces.json').read_bytes()), result)
        self.assertEqual(self.source.read_bytes(), self.raw)

    def test_all_ten_inverse_semantic_audits_and_unedited_xml_bytes(self):
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

    def test_rehashed_plan_cannot_change_source_order_role_block_or_pairs(self):
        for mutation in ('source', 'order', 'role', 'block', 'position', 'pairs', 'count', 'duplicate', 'target'):
            plan = copy.deepcopy(self.plan)
            if mutation == 'source':
                plan['source_workspace_sha256'] = '0' * 64
            elif mutation == 'order':
                plan['profiles'][1:5] = reversed(plan['profiles'][1:5])
            elif mutation == 'role':
                plan['profiles'][7]['ordinary_role'] = 'new_prediction_check'
            elif mutation == 'block':
                plan['profiles'][5]['block'] = 'a'
            elif mutation == 'position':
                plan['profiles'][1]['block_position'] = True
            elif mutation == 'pairs':
                plan['order_comparisons']['pairs'].reverse()
            elif mutation == 'count':
                plan['profiles'].pop()
            elif mutation == 'duplicate':
                plan['profiles'][-1]['name'] = plan['profiles'][0]['name']
            else:
                plan['profiles'][-1]['ordinary_output_target_dbm'] = 0
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.prepare(plan)
            self.assertFalse(self.output.exists())

    def test_late_profile_physics_failure_precedes_all_destination_writes(self):
        for field, value in (('source_phase_deg', 0), ('gain_db', 21), ('sweep_step_db', 0),
                             ('power_dbm', [0, 10, 20]), ('phase_deg', [10, 40, -21]),
                             ('sweep_input_stop_dbm', False)):
            plan = copy.deepcopy(self.plan)
            plan['profiles'][-1][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.prepare(plan)
            self.assertFalse(self.output.exists())

    def test_same_target_byte_mismatch_is_rejected_before_writing(self):
        original = m.prepare_document

        def divergent(raw, profile):
            data, changes = original(raw, profile)
            return (data + b' ' if profile['name'] == 'b-zero' else data), changes

        with mock.patch.object(m, 'prepare_document', divergent), self.assertRaisesRegex(ValueError, 'A/B prepared workspace bytes differ'):
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

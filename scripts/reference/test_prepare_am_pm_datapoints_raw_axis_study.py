"""Synthetic XML preparation guards; no vendor workspace or COM required."""
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

SCRIPT = Path(__file__).with_name('prepare-am-pm-datapoints-raw-axis-study.py')
spec = importlib.util.spec_from_file_location('prepare_raw_axis', SCRIPT)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


# This contract is independent of the implementation's path builder. It also
# drives the synthetic tree, so a misspelled production path cannot confirm
# itself by silently changing the test fixture in the same way.
EXPECTED_FIELDS = {
    'DataPoints/Sch2/PartList/RFAmp/ParamSet/AMtoPM_Mode/DataEntry': '2',
    'DataPoints/Sch2/PartList/RFAmp/ParamSet/G/DataEntry': '20',
    'DataPoints/Sch2/PartList/RFAmp/ParamSet/OP1dB/DataEntry': '100',
    'DataPoints/Sch2/PartList/RFAmp/ParamSet/OPSAT/DataEntry': '103',
    'DataPoints/Sch2/PartList/RFAmp/ParamSet/OIP2/DataEntry': '140',
    'DataPoints/Sch2/PartList/RFAmp/ParamSet/OIP3/DataEntry': '130',
    'DataPoints/Sch2/PartList/RFAmp/ParamSet/PwrAMtoPMpoints/DataEntry': '[0,10,20]',
    'DataPoints/Sch2/PartList/RFAmp/ParamSet/AMtoPMpoints/DataEntry': '[10,40,-20]',
    'DataPoints/Sch2/PartList/Source/ParamSet/Pwr/DataEntry': '-19.999',
    'DataPoints/Sch2/PartList/Source/ParamSet/Phase/DataEntry': '0',
    'DataPoints/Sch2/PartList/Source/ParamSet/Freq/DataEntry': '100',
    'DataPoints/System2/Path1/QCStartPwr/DataEntry': '-20.01',
    'DataPoints/System2/Path1/QCStopPwr/DataEntry': '-19.99',
    'DataPoints/System2/Path1/QCStepPwr/DataEntry': '0.001',
    'DataPoints/System2/Path1/QCFreq/DataEntry': '100',
    'DataPoints/System2/Path1/QCAutoLimits/Data/D': '0',
    'DataPoints/System2/Path1/QCSwpType/Data/D': '3',
}


def profile():
    return dict(name='tiny-positive', power_dbm=[0, 10, 20], phase_deg=[10, 40, -20],
                gain_db=20, output_p1db_dbm=100, output_saturation_dbm=103,
                output_ip2_dbm=140, output_ip3_dbm=130, ordinary_output_target_dbm=.001,
                source_phase_deg=0, frequency_hz=1e8, sweep_input_start_dbm=-20.01,
                sweep_input_stop_dbm=-19.99, sweep_step_db=.001)


def fixture():
    root = ET.Element('Workspace')
    for path in EXPECTED_FIELDS:
        current = root
        for name in path.split('/'):
            if name in ('DataEntry', 'Data', 'D'):
                child = current.find(name)
                if child is None:
                    child = ET.SubElement(current, name)
            else:
                child = next((c for c in current.findall('Item') if c.get('XName') == name), None)
                if child is None:
                    child = ET.SubElement(current, 'Item', XName=name)
            current = child
        current.text = None if path.endswith('QCFreq/DataEntry') else 'old-expression'
    raw = ET.tostring(root, encoding='utf-8').replace(b'</Item>', b'</Item>\r\n<!-- keep spacing -->')
    raw = raw.replace(b'</Workspace>', b'<Unchanged key="one &amp; two">&#65;</Unchanged></Workspace>')
    return b'<?xml version="1.0" encoding="utf-8"?>\r\n<!-- keep exact bytes -->\r\n' + raw + b'\r\n'


class PreparationTests(unittest.TestCase):
    def test_independent_seventeen_field_expression_contract(self):
        self.assertEqual(len(EXPECTED_FIELDS), 17)
        self.assertEqual(m.planned_fields(profile()), EXPECTED_FIELDS)
        _, changes = m.prepare_document(fixture(), profile())
        self.assertEqual({change['path']: change['after'] for change in changes}, EXPECTED_FIELDS)

    def test_seventeen_changes_and_unchanged_bytes(self):
        raw = fixture()
        data, changes = m.prepare_document(raw, profile())
        self.assertEqual(len(changes), 17)
        self.assertTrue(data.startswith(raw.split(b'<Workspace>')[0]))
        self.assertTrue(data.endswith(b'\r\n'))
        self.assertEqual(m.element(ET.fromstring(data), 'DataPoints/Sch2/PartList/Source/ParamSet/Pwr/DataEntry').text,
                         '-19.999')
        mask = lambda payload: re.sub(rb'<(?:DataEntry|D)(?:\s*/>|>[^<]*</(?:DataEntry|D)>)',
                                      b'<edited/>', payload)
        self.assertEqual(mask(data), mask(raw))
        self.assertEqual(raw, fixture())

    def test_tiny_decimal_source_never_cancels_to_zero_offset(self):
        self.assertEqual(m.source_expression('0.0000000000000000000000000000000000000001', 20),
                         '-19.9999999999999999999999999999999999999999')
        self.assertEqual(m.source_expression('-0.001', 20), '-20.001')
        self.assertEqual(m.planned_fields(profile())['DataPoints/System2/Path1/QCStepPwr/DataEntry'], '0.001')

    def test_invalid_profile_and_duplicate_xml_rejected(self):
        for name in ('../escape', 'a/b', 'C:escape', 'CON', 'com1'):
            p = profile(); p['name'] = name
            with self.subTest(name=name), self.assertRaises(ValueError):
                m.prepare_document(fixture(), p)
        p = profile(); p['power_dbm'] = [0, 0, 20]
        with self.assertRaises(ValueError): m.prepare_document(fixture(), p)
        raw = fixture().replace(b'<DataEntry>old-expression</DataEntry>',
                                b'<DataEntry>x</DataEntry><DataEntry>y</DataEntry>', 1)
        with self.assertRaises(ValueError): m.prepare_document(raw, profile())

    def test_output_scope_hash_and_overwrite_guards(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)/'root'; root.mkdir()
            source = root/m.SOURCE_PATH; source.parent.mkdir(parents=True); source.write_bytes(fixture())
            plan = root/'plan.json'; output = root/'new'
            document = dict(source_workspace_sha256=m.digest(fixture()), profiles=[profile()])
            plan.write_text(json.dumps(document), encoding='utf-8')
            with mock.patch.object(m, 'SOURCE_SHA256', m.digest(fixture())), \
                    mock.patch.object(m, 'PLAN_SHA256', m.digest(plan.read_bytes())):
                for bad in (root, root.parent/'escape'):
                    with self.assertRaises(ValueError): m.prepare(root, plan, bad)
                source.write_bytes(fixture()+b' ')
                with self.assertRaises(ValueError): m.prepare(root, plan, output)
                self.assertFalse(output.exists())
                source.write_bytes(fixture())
                result = m.prepare(root, plan, output)
                self.assertEqual(len(result['profiles']), 1)
                captured = (output/'prepared-workspaces.json').read_bytes()
                with self.assertRaises(ValueError): m.prepare(root, plan, output)
                self.assertEqual((output/'prepared-workspaces.json').read_bytes(), captured)
                document['source_workspace_sha256'] = '0'*64
                plan.write_text(json.dumps(document), encoding='utf-8')
                with self.assertRaises(ValueError): m.prepare(root, plan, root/'other')
                self.assertFalse((root/'other').exists())
                with mock.patch.object(m, 'PLAN_SHA256', m.digest(plan.read_bytes())):
                    with self.assertRaisesRegex(ValueError, 'Plan source hash changed'):
                        m.prepare(root, plan, root/'other')

    def test_all_profiles_are_validated_before_any_files_are_created(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root/m.SOURCE_PATH
            source.parent.mkdir(parents=True)
            source.write_bytes(fixture())
            valid, invalid = profile(), profile()
            invalid['name'] = 'invalid'; invalid['sweep_step_db'] = 0
            plan = root/'plan.json'
            plan.write_text(json.dumps(dict(source_workspace_sha256=m.digest(fixture()),
                                            profiles=[valid, invalid])), encoding='utf-8')
            with mock.patch.object(m, 'SOURCE_SHA256', m.digest(fixture())), \
                    mock.patch.object(m, 'PLAN_SHA256', m.digest(plan.read_bytes())):
                with self.assertRaises(ValueError): m.prepare(root, plan, root/'output')
            self.assertFalse((root/'output').exists())


if __name__ == '__main__':
    unittest.main()

"""Semantic corruption checks for the archived RES evidence replay."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import xml.etree.ElementTree as ET

spec = importlib.util.spec_from_file_location('res_replay', Path(__file__).with_name('replay-systemvue-res-evidence.py'))
replay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(replay)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    args = parser.parse_args()
    files = replay.load_archive(args.archive, args.manifest)
    baseline = replay.score(files)
    checks = []

    def reject(name, member, mutate, fail_score=False):
        changed = dict(files)
        document = replay.read_json(files[member])
        mutate(document)
        changed[member] = json.dumps(document, allow_nan=False).encode('utf-8')
        try:
            result = replay.score(changed)
        except (ValueError, KeyError):
            checks.append(name)
            return
        if fail_score and result['counts']['fail'] > 0:
            checks.append(name)
            return
        raise AssertionError('invalid evidence accepted: ' + name)

    def variable(document, suffix, name):
        capture = replay.Capture(document, document['readback']['workspace'])
        return capture.variables(suffix)[name]

    linear_member = 'vendor/linear-state.json'
    rf_member = 'vendor/rf-state.json'
    linear_base = 'Designs/linear_once_Data/Eqns/VarBlock/'
    rf_base = 'Designs/RES_RFSystem_Data_Folder/rf_once_Data_RES_Path/Eqns/VarBlock/'
    def set_value(raw, value):
        replay.unwrap(raw)['value'] = value
    def replace_double(raw, value):
        raw = replay.unwrap(raw)
        raw.update(value=value, roundtrip_text=repr(value), binary64_hex=struct.pack('>d', value).hex())

    reject('wrong_vendor_build', linear_member, lambda d: d.update(installed_executable_version='unknown'))
    reject('unknown_snapshot_equality', linear_member, lambda d: d['readback'].update(xml_before_after_equal=None))
    reject('truncated_traversal', linear_member, lambda d: d['readback'].update(capture_complete=False))
    reject('multiple_analysis_submissions', 'vendor/linear-once.json', lambda d: d['run'].update(analysis_requests=2))
    reject('wrong_run_dataset', 'vendor/rf-once.json', lambda d: d['run'].update(dataset='other_Data'))
    reject('S_alias_wrong_port_order', linear_member,
           lambda d: set_value(variable(d, linear_base + 'S12', 'DataEntry'), '=S(:,2,1)'))
    reject('S_unit_metadata_changed', linear_member,
           lambda d: set_value(variable(d, linear_base + 'S11', 'Unit'), 9007))
    reject('analysis_dataset_binding_changed', linear_member,
           lambda d: set_value(replay.Capture(d, 'linear-once-result').node('Designs/RES_Linear/DataName')['parameter_reads']['GetValue'], 'other_Data'))
    reject('runner_source_hash_changed', 'vendor/linear-once.json', lambda d: d.update(script_sha256='0' * 64))
    reject('collector_source_hash_changed', linear_member, lambda d: d.update(collector_sha256='0' * 64))
    reject('run_dataset_XML_changed', 'vendor/linear-once.json',
           lambda d: d['run'].update(result_xml=d['run']['result_xml'].replace('1792152018', '0')))

    def run_xml_value(document, field, item_names, property_path, value):
        root = ET.fromstring(document['run'][field])
        node = root
        for name in item_names:
            found = [child for child in node if child.tag == 'Item' and child.get('XName') == name]
            assert len(found) == 1
            node = found[0]
        entry = node.find(property_path)
        assert entry is not None
        entry.text = value
        document['run'][field] = ET.tostring(root, encoding='unicode')

    part_path = ['Designs', 'RES_Shared', 'PartList']
    reject('submitted_resistance_changed', 'vendor/linear-once.json',
           lambda d: run_xml_value(d, 'input_xml', part_path + ['R1', 'ParamSet', 'R'], 'DataEntry', '100'))
    reject('submitted_wiring_changed', 'vendor/linear-once.json',
           lambda d: run_xml_value(d, 'input_xml', part_path + ['R1'], 'Netlist', 'Port_1,1|Port_2,2'))
    reject('submitted_source_power_changed', 'vendor/rf-once.json',
           lambda d: run_xml_value(d, 'input_xml', part_path + ['Source', 'ParamSet', 'Pwr'], 'DataEntry', '3'))
    reject('submitted_load_changed', 'vendor/rf-once.json',
           lambda d: run_xml_value(d, 'input_xml', part_path + ['Load', 'ParamSet', 'ZO'], 'DataEntry', '100'))
    reject('submitted_linear_noise_flag_changed', 'vendor/linear-once.json',
           lambda d: run_xml_value(d, 'input_xml', ['Designs', 'RES_Linear'], 'CalcNoise/D', '1'))
    reject('submitted_rf_harmonics_flag_changed', 'vendor/rf-once.json',
           lambda d: run_xml_value(d, 'input_xml', ['Designs', 'RES_RFSystem'], 'CalcHarmonics/D', '1'))
    reject('submitted_path_definition_changed', 'vendor/rf-once.json',
           lambda d: run_xml_value(d, 'input_xml', ['Designs', 'RES_RFSystem', 'Path0'], 'PathDef', 'Load,Source'))
    reject('submitted_frequency_unit_changed', 'vendor/linear-once.json',
           lambda d: run_xml_value(d, 'input_xml', ['Designs', 'RES_Linear', 'Start'], 'Unit/D', '1001'))
    reject('submitted_pending_state_changed', 'vendor/linear-once.json',
           lambda d: run_xml_value(d, 'input_xml', ['Designs', 'RES_Linear'], 'NeedRun/D', '0'))
    reject('completed_state_changed', 'vendor/linear-once.json',
           lambda d: run_xml_value(d, 'result_xml', ['Designs', 'RES_Linear'], 'NeedRun/D', '1'))
    reject('run_result_load_changed', 'vendor/rf-once.json',
           lambda d: run_xml_value(d, 'result_xml', part_path + ['Load', 'ParamSet', 'ZO'], 'DataEntry', '100'))
    reject('preflight_parts_disagree_with_input', 'vendor/linear-once.json',
           lambda d: run_xml_value(d, 'part_snapshot', ['R1'], 'Netlist', 'Port_1,1|Port_2,2'))
    reject('submitted_model_name_whitespace_changed', 'vendor/linear-once.json',
           lambda d: run_xml_value(d, 'input_xml', part_path + ['R1'], 'Model', 'RES '))
    reject('submitted_dataset_name_whitespace_changed', 'vendor/linear-once.json',
           lambda d: run_xml_value(d, 'input_xml', ['Designs', 'RES_Linear', 'DataName'], 'Data', 'linear_once_Data '))

    def run_xml_tail(document):
        root = ET.fromstring(document['run']['input_xml'])
        node = next(child for child in root if child.tag == 'Item' and child.get('XName') == 'Designs')
        design = next(child for child in node if child.tag == 'Item' and child.get('XName') == 'RES_Shared')
        design[0].tail = 'not_indentation'
        document['run']['input_xml'] = ET.tostring(root, encoding='unicode')
    reject('submitted_nonwhitespace_XML_tail', 'vendor/linear-once.json', run_xml_tail)

    def runtime_state_tail(document):
        root = ET.fromstring(document['run']['input_xml'])
        designs = next(child for child in root if child.tag == 'Item' and child.get('XName') == 'Designs')
        analysis = next(child for child in designs if child.tag == 'Item' and child.get('XName') == 'RES_Linear')
        analysis.find('NeedRun').tail = 'not_indentation'
        document['run']['input_xml'] = ET.tostring(root, encoding='unicode')
    reject('removed_NeedRun_nonwhitespace_tail', 'vendor/linear-once.json', runtime_state_tail)

    # Formatting alone is permitted; physical string values are never trimmed.
    formatted = dict(files)
    document = replay.read_json(files['vendor/linear-once.json'])
    root = ET.fromstring(document['run']['input_xml'])
    ET.indent(root, space='    ')
    document['run']['input_xml'] = ET.tostring(root, encoding='unicode')
    formatted['vendor/linear-once.json'] = json.dumps(document, allow_nan=False).encode('utf-8')
    assert replay.score(formatted) == baseline
    reject('double_unit_conversion', rf_member,
           lambda d: set_value(variable(d, rf_base + 'DCV', 'Unit'), 11000))
    reject('wrong_node_dependency', rf_member,
           lambda d: set_value(variable(d, rf_base + 'DCP', 'IndepValue'), 'DifferentNodeNames'))
    reject('wrong_lower_bound', rf_member,
           lambda d: replay.unwrap(variable(d, rf_base + 'DCP', 'Data')).update(lower_bounds=[1]))
    reject('binary64_text_disagrees', linear_member,
           lambda d: replay.unwrap(variable(d, linear_base + 'S11', 'Data')).update(roundtrip_text='0.25'))
    reject('display_string_in_place_of_binary64', linear_member,
           lambda d: replay.unwrap(variable(d, linear_base + 'S11', 'Data')).update(clr_type='System.String'))
    reject('wrong_physical_wiring', linear_member,
           lambda d: set_value(variable(d, 'Designs/RES_Shared/PartList/R1', 'Netlist'), 'Port_1,1|Port_2,2'))

    def wrong_s(document):
        capture = replay.Capture(document, 'linear-once-result')
        replace_double(capture.variables(linear_base + 'S11')['Data'], -.5)
        replace_double(capture.node(linear_base + 'S11')['parameter_reads']['GetValue'], -.5)
    reject('wrong_reflection_sign_is_failure', linear_member, wrong_s, True)
    reject('wrong_native_reference_plane', 'native/Release-native.json',
           lambda d: d['path_power'].update(source_available_w=.00075))
    for name, raw in [('duplicate_JSON_key', b'{"a":1,"a":2}'), ('nonfinite_JSON', b'{"a":NaN}')]:
        try:
            replay.read_json(raw)
        except ValueError:
            checks.append(name)
        else:
            raise AssertionError(name)
    require_counts = baseline['counts']
    assert require_counts == {'pass': 5, 'fail': 0, 'not_comparable': 5}
    assert baseline['full_minimum_accepted'] is False
    print(json.dumps({'status': 'pass', 'negative_checks_passed': len(checks), 'checks': checks,
                      'formatting_preservation_checks_passed': 1,
                      'baseline_counts': require_counts}, indent=2))


if __name__ == '__main__':
    main()

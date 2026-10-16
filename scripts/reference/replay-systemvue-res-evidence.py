"""Replay the frozen RES experiment from retained evidence, without SystemVue.

This is a deliberately bounded experiment scorer, not a generic COM importer.
Unresolved source or complex-array identity remains not_comparable. Numeric
proximity never supplies missing metadata. The archive manifest binds all bytes.
"""
import argparse
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

PLAN_SHA256 = '4000c4549f5ed0a129f1f5ccf1069d4ef4c36be6da9c5de04d86b441f9d4a30e'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def pairs(items):
    result = {}
    for key, value in items:
        require(key not in result, 'duplicate JSON key: ' + key)
        result[key] = value
    return result


def read_json(raw):
    return json.loads(raw.decode('utf-8-sig'), object_pairs_hook=pairs,
                      parse_constant=lambda token: (_ for _ in ()).throw(ValueError(token)))


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def unwrap(raw):
    if 'ok' in raw:
        require(raw['ok'] is True, 'failed raw read')
        raw = raw['result']
    return raw


def scalar(raw, double=False):
    raw = unwrap(raw)
    require(raw['kind'] == 'scalar', 'expected scalar')
    value = raw['value']
    if double:
        require(raw['clr_type'] == 'System.Double' and raw['finite'] is True,
                'expected finite binary64, not a display string or array')
        require(type(value) in (int, float) and math.isfinite(value), 'nonfinite scalar')
        require(struct.pack('>d', value).hex() == raw['binary64_hex'].lower(), 'binary64 bits differ')
        require(struct.pack('>d', float(raw['roundtrip_text'])) == struct.pack('>d', value),
                'roundtrip text differs')
    return value


def vector(raw, count, double=True):
    raw = unwrap(raw)
    require(raw['kind'] == 'array' and raw['rank'] == 1 and raw['lengths'] == [count]
            and raw['lower_bounds'] == [0], 'unexpected vector shape or lower bounds')
    entries = raw['indexed_values']
    require(len(entries) == count, 'vector length differs')
    require([e['indices'] for e in entries] == [[i] for i in range(count)], 'vector indices differ')
    return [scalar(e['value'], double) for e in entries]


class Capture:
    def __init__(self, document, workspace):
        self.document = document
        self.readback = document['readback']
        require(document['installed_executable_version'] == '2023.0.0.11903', 'unexpected vendor build')
        r = self.readback
        require(r['schema_version'] == 2 and r['capture_complete'] is True, 'incomplete snapshot')
        require(r['workspace'] == workspace == r['expected_workspace'], 'wrong snapshot workspace')
        require(r['analysis_requested'] is False and r['explicit_mutation_calls'] == 0,
                'capture was not readback only')
        require(r['xml_before_after_equal'] is True and r['errors_before_after_equal'] is True,
                'snapshot changed while reading')
        for key in ('errors_before', 'errors_after'):
            require(scalar(r[key]) == '', 'vendor errors in snapshot')
        for key in ('analysis_done_before', 'analysis_done_after'):
            require(scalar(r[key]) == 1, 'analysis not complete')
        self.nodes = {}
        def walk(node):
            require(node['path'] not in self.nodes, 'duplicate tree path')
            self.nodes[node['path']] = node
            for child in node.get('children', []):
                walk(child)
        walk(r['tree'])
        require(len(self.nodes) == r['nodes_read'], 'traversal count differs')

    def node(self, suffix):
        found = [n for path, n in self.nodes.items() if path.endswith('/' + suffix)]
        require(len(found) == 1, 'missing or ambiguous node: ' + suffix)
        return found[0]

    def variables(self, suffix):
        result = {}
        for variable in self.node(suffix)['variables']:
            name = scalar(variable['xml_name'])
            require(name not in result, 'duplicate variable')
            result[name] = variable['value']
        return result

    def value(self, suffix, name):
        return scalar(self.variables(suffix)[name])

    def parameter(self, suffix):
        return scalar(self.node(suffix)['parameter_reads']['GetValue'])

    def data(self, suffix, count=None):
        variables = self.variables(suffix)
        require(scalar(variables['ByAnalysis']) == 1, 'not analysis-produced data')
        raw = variables['Data']
        read = self.node(suffix)['parameter_reads']['GetValue']
        decode = (lambda x: scalar(x, True)) if count is None else (lambda x: vector(x, count))
        actual = decode(raw)
        require(actual == decode(read), 'stored data and GetValue disagree')
        return actual


def run_identity(document, analysis, dataset):
    require(document['plan_sha256'] == PLAN_SHA256, 'run plan mismatch')
    run = document['run']
    require(run['analysis'] == analysis and run['dataset'] == dataset, 'wrong run identity')
    require(run['analysis_requests'] == 1 and run['analysis_done'] is True, 'not one completed run')
    require(run['exception'] is None and run['errors_before'] == run['errors_after'] == '', 'run failed')


def fixture(capture):
    prefix = 'Designs/RES_Shared/PartList/'
    require({n['name'] for n in capture.node(prefix.rstrip('/'))['children']} ==
            {'PartLib', 'Source', 'R1', 'R2', 'Load'}, 'unexpected additional device or part-list object')
    for name, model, netlist in [('Source', 'MultiSource', '1,1'), ('R1', 'RES', 'Port_1,1|Port_2,3'),
                                ('R2', 'RES', 'Port_1,3|Port_2,2'), ('Load', '*OUT', 'Port_1,2')]:
        require(capture.value(prefix + name, 'Model') == model, 'wrong device model')
        require(capture.value(prefix + name, 'Netlist') == netlist, 'changed physical wiring')
    for name in ('R1', 'R2'):
        require(capture.parameter(prefix + name + '/ParamSet/R') == 50, 'wrong resistance')
        require(capture.parameter(prefix + name + '/ParamSet/Ta') == '', 'temperature override')
    for name, expectations in [('Source', {'Freq': 1e8, 'BW': 1, 'Pwr': .001, 'Phase': 0,
                                          'R': 50, 'ZO': 50, 'PORT': 1, 'Enable': 1, 'SrcType': 0,
                                          'MultiCarrier': 0, 'EnablePN': 0, 'Desired': 1, 'POff': 1, 'PhOff': 0}),
                                ('Load', {'PORT': 2, 'ZO': 50})]:
        for field, value in expectations.items():
            require(capture.parameter(prefix + name + '/ParamSet/' + field) == value,
                    'source/load setting differs: ' + field)
    for name in ('RES_Linear', 'RES_RFSystem'):
        suffix = 'Designs/' + name
        require(capture.value(suffix, 'AutoRecalc') == capture.value(suffix, 'CalcNoise') == 0,
                'automatic calculation or noise enabled')
        require(capture.parameter(suffix + '/DesignName') == 'RES_Shared', 'different DUT')
        require(capture.parameter(suffix + '/Temperature') == 26.85, 'analysis temperature differs')
    for name, value in {'Start': 1e8, 'Stop': 1e8, 'NumPoints': 1}.items():
        require(capture.parameter('Designs/RES_Linear/' + name) == value, 'linear sweep setting differs')


def xml_item(raw, names):
    node = ET.fromstring(raw)
    for name in names:
        found = [child for child in node if child.tag == 'Item' and child.get('XName') == name]
        require(len(found) == 1, 'item missing or ambiguous in XML: ' + name)
        node = found[0]
    return node


def normalized_xml(element):
    # Only inter-element whitespace is formatting. Preserve complete leaf text
    # (including string/parameter whitespace) and any non-whitespace mixed text
    # or child tail; stripping every value would weaken the identity binding.
    text = element.text or ''
    if len(element) and not text.strip():
        text = ''
    tail = element.tail or ''
    return (element.tag, sorted(element.attrib.items()), text,
            tail if tail.strip() else '', [normalized_xml(child) for child in element])


def dataset_xml(raw, names):
    node = xml_item(raw, names)
    # SaveAs/readback materializes FullPath at the dataset root after RunAnalysis.
    # Permit only that exact derived identity field; compare all data, equations,
    # timestamps and other attributes without exclusions.
    paths = node.findall('FullPath')
    require(len(paths) <= 1, 'duplicate dataset FullPath')
    if paths:
        require(paths[0].text == '\\'.join(names) and not paths[0].attrib and not len(paths[0])
                and not (paths[0].tail or '').strip(), 'dataset FullPath differs')
        node.remove(paths[0])
    return normalized_xml(node)


def submitted_fixture(run, analysis, capture):
    # Bind the already validated captured fixture to the actual submitted input,
    # not only to the saved result dataset. With AutoRecalc disabled, a later
    # schematic edit can otherwise coexist with an unchanged earlier dataset.
    # These owned snapshots retain the complete DUT and selected analysis, so
    # no parameter-expression evaluator or duplicate physical-setting table is
    # needed. A different expression/unit/wire/flag is a different submission.
    captured = scalar(capture.readback['xml_before'])
    design_path = ['Designs', 'RES_Shared']
    design = normalized_xml(xml_item(captured, design_path))
    for name, raw in [('input', run['input_xml']), ('result', run['result_xml']),
                      ('capture_after', scalar(capture.readback['xml_after']))]:
        require(normalized_xml(xml_item(raw, design_path)) == design,
                'submitted DUT differs from validated capture: ' + name)
    require(normalized_xml(ET.fromstring(run['part_snapshot'])) ==
            normalized_xml(xml_item(run['input_xml'], design_path + ['PartList'])),
            'preflight part snapshot differs from submitted input')

    def settings(raw, pending):
        node = xml_item(raw, ['Designs', analysis])
        states = node.findall('NeedRun')
        expected = ET.fromstring('<NeedRun><T>Bool</T><D>' + str(pending) + '</D></NeedRun>')
        require(len(states) == 1 and normalized_xml(states[0]) == normalized_xml(expected)
                and not (states[0].tail or '').strip(),
                'unexpected analysis NeedRun state')
        # Only this observed runtime transition is allowed: pending input (1)
        # becomes completed output (0). Do not erase other analysis properties.
        node.remove(states[0])
        return normalized_xml(node)

    completed = settings(captured, 0)
    require(settings(run['input_xml'], 1) == completed,
            'submitted analysis settings differ from validated capture')
    for raw in (run['result_xml'], scalar(capture.readback['xml_after'])):
        require(settings(raw, 0) == completed,
                'completed analysis settings differ from validated capture')


def score(files):
    plan = read_json(files['plan.json'])
    require(sha(files['plan.json']) == PLAN_SHA256, 'changed frozen policy')
    linear = Capture(read_json(files['vendor/linear-state.json']), 'linear-once-result')
    rf = Capture(read_json(files['vendor/rf-state.json']), 'rf-once-result')
    for capture in (linear, rf):
        fixture(capture)
    for stem, analysis in [('linear', 'RES_Linear'), ('rf', 'RES_RFSystem')]:
        run = read_json(files['vendor/' + stem + '-once.json'])
        run_identity(run, analysis, stem + '_once_Data')
        require(sha(files['vendor/sources/run-systemvue-res-analysis-acquired.ps1']) == run['script_sha256'],
                'acquisition source hash differs')
        capture = linear if stem == 'linear' else rf
        submitted_fixture(run['run'], analysis, capture)
        require(capture.parameter('Designs/' + analysis + '/DataName') == stem + '_once_Data',
                'analysis dataset parameter differs')
        names = (['Designs', 'linear_once_Data'] if stem == 'linear' else
                 ['Designs', 'RES_RFSystem_Data_Folder', 'rf_once_Data_RES_Path'])
        require(dataset_xml(run['run']['result_xml'], names) ==
                dataset_xml(scalar(capture.readback['xml_before']), names),
                'captured dataset differs from original run XML')
    for capture in (linear, rf):
        require(sha(files['vendor/sources/capture-systemvue-res-state-acquired.ps1']) == capture.document['collector_sha256'],
                'capture source hash differs')
    native = read_json(files['native/Release-native.json'])
    native_verifier_path = Path(__file__).parent / 'behavioral-res-probe' / 'verify.py'
    spec = importlib.util.spec_from_file_location('res_native_verify', native_verifier_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory(prefix='rfmodel-res-oracle-') as directory:
        p, n = Path(directory) / 'plan.json', Path(directory) / 'native.json'
        p.write_bytes(files['plan.json'])
        n.write_bytes(files['native/Release-native.json'])
        native_check = module.verify(p, n)

    results = []
    thresholds = plan['comparison_policy']['thresholds']
    def compare(name, actual, expected, native_value, threshold, provenance):
        limit = thresholds[threshold]['atol'] + thresholds[threshold]['rtol'] * abs(expected)
        delta, native_delta = abs(actual - expected), abs(actual - native_value)
        encode = lambda v: {'real': v.real, 'imag': v.imag} if isinstance(v, complex) else v
        results.append({'id': name, 'status': 'pass' if max(delta, native_delta) <= limit else 'fail',
                        'vendor': encode(actual), 'oracle': encode(expected), 'native': encode(native_value),
                        'vendor_oracle_absolute_error': delta, 'vendor_native_absolute_error': native_delta,
                        'limit': limit, 'threshold': threshold, 'provenance': provenance})
    base = 'Designs/linear_once_Data/Eqns/VarBlock/'
    require(linear.value(base + 'F', 'Unit') == 1003, 'frequency unit metadata differs')
    compare('linear_frequency', linear.data(base + 'F', 1)[0], 1e8,
            native['linear']['frequency_hz'], 'frequency_hz', base + 'F: Data/GetValue in Hz')
    for i, name in enumerate(('S11', 'S12', 'S21', 'S22')):
        row, column = int(name[1]), int(name[2])
        require(linear.value(base + name, 'DataEntry') == f'=S(:,{row},{column})', 'S alias formula differs')
        require(linear.value(base + name, 'Unit') == 13002, 'S unit metadata differs')
        require(linear.value(base + name, 'IndepValue') == r'Designs\linear_once_Data\Eqns\VarBlock\F',
                'S frequency dependency differs')
        raw_native = native['linear']['S_row_major'][i]
        compare('linear_' + name, complex(linear.data(base + name), 0), complex(.5, 0),
                complex(raw_native['real'], raw_native['imag']), 'dimensionless_complex_S',
                base + name + ': complete System.Double value and explicit S(:,row,column) alias')
    # Complex SAFEARRAY axes are never decoded by numeric resemblance. A later
    # explicit real()/imag() export may close these two identities independently.
    for port in (1, 2):
        results.append({'id': 'linear_ZPORT_' + str(port), 'status': 'not_comparable',
                        'reason': 'Complex SAFEARRAY port/component axes have no confirmed contract.'})

    path = 'Designs/RES_RFSystem/Path0'
    require(rf.value(path, 'PathDef') == 'Source,Load', 'wrong path definition')
    for key, value in {'Internal': 0, 'AddPwrMeas': 1, 'AddVoltMeas': 1, 'CalcVolt': 1,
                       'PathFreq': 1e8, 'Enable': 1}.items():
        require(rf.parameter(path + '/' + key) == value, 'path setting differs: ' + key)
    rf_analysis = 'Designs/RES_RFSystem'
    for key in ('CalcHarmonics', 'CalcIntermods', 'CalcPhaseNoise'):
        require(rf.value(rf_analysis, key) == 0, 'nonlinear/noise calculation enabled')
    require(rf.parameter(rf_analysis + '/ChanBW') == 1000, 'channel bandwidth differs')
    require(rf.parameter(rf_analysis + '/UseSpecReduction') == 0, 'spectrum reduction enabled')
    path_data = 'Designs/RES_RFSystem_Data_Folder/rf_once_Data_RES_Path/Eqns/VarBlock/'
    nodes = vector(rf.variables(path_data + 'NodeNames')['Data'], 3, False)
    require(nodes == ['1', '3', '2'], 'resolved node order differs')
    require(rf.data(path_data + 'CF', 3) == [1e8] * 3, 'path frequencies differ')
    observations = {}
    for name, unit, description in [('DCP', 11000, 'Desired Channel Power'),
                                    ('DCV', 9007, 'Desired Channel Voltage')]:
        require(rf.value(path_data + name, 'Unit') == unit, 'measurement unit metadata differs')
        require(rf.value(path_data + name, 'Description') == description, 'measurement identity differs')
        require(rf.value(path_data + name, 'IndepValue') ==
                r'Designs\RES_RFSystem_Data_Folder\rf_once_Data_RES_Path\Eqns\VarBlock\NodeNames',
                'measurement node dependency differs')
        observations[name] = rf.data(path_data + name, 3)
    for name in ('rf_DCP_first_node', 'rf_DCP_output', 'rf_DCV_output'):
        results.append({'id': name, 'status': 'not_comparable',
                        'reason': 'Observed SrcType=0 is not yet bound to an authoritative CW enumeration.'})
    counts = {status: sum(row['status'] == status for row in results)
              for status in ('pass', 'fail', 'not_comparable')}
    return {'schema_version': 1, 'plan_sha256': PLAN_SHA256,
            'status': 'accepted_minimum' if counts['pass'] == 10 else 'incomplete_minimum',
            'full_minimum_accepted': counts['pass'] == 10, 'vendor_analysis_requests': 2,
            'eligible_atomic_comparisons': 10, 'counts': counts,
            'native_oracle_checks': native_check['checks_passed'], 'comparisons': results,
            'unscored_raw_rf_linear_units': observations,
            'limitations': ['One 100-MHz two-RES fixture only.',
                            'No noise, arbitrary CP/CV, current-direction, nonlinear or complex-reference acceptance.']}


def load_archive(archive, manifest):
    manifest = read_json(Path(manifest).read_bytes())
    require(sha(Path(archive).read_bytes()) == manifest['archive_sha256'], 'archive hash differs')
    with zipfile.ZipFile(archive) as bundle:
        names = bundle.namelist()
        require(len(names) == len(set(names)), 'duplicate archive member')
        declared = {entry['path']: entry for entry in manifest['files']}
        require(len(declared) == len(manifest['files']) and set(names) == set(declared), 'manifest member set differs')
        files = {}
        for name in names:
            require(not name.startswith('/') and '..' not in Path(name).parts, 'unsafe member name')
            raw = bundle.read(name)
            require(len(raw) == declared[name]['bytes'] and sha(raw) == declared[name]['sha256'], 'member hash differs: ' + name)
            files[name] = raw
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    result = score(load_archive(args.archive, args.manifest))
    text = json.dumps(result, indent=2, allow_nan=False) + '\n'
    if args.out:
        with args.out.open('x', encoding='utf-8', newline='\n') as stream:
            stream.write(text)
    print(text)
    if result['counts']['fail']:
        sys.exit(1)


if __name__ == '__main__':
    main()

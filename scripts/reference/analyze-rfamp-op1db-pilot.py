"""Freeze A/B identification and a prospective C predictor; score C without refitting.

Developer evidence only. Native C++ computes the existing common response and
coherent polynomial. No SystemVue command is submitted by this program.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from native_replay import NativeReplay
from reference_records import CoherentComponent, SpectrumKind

ORDERS = (5, 7, 9, 11)
TOLERANCE = 1e-7
UNIT_SOURCE = HERE / 'capture-cascade-parameter-units.ps1'
LOGIC_SOURCES = (
    'analyze-rfamp-op1db-pilot.py', 'rfamp-parameter-pilot.py',
    'archive-cascade-reference.py', 'archive-cascade-input-reference.py',
    'compare-cascade-amplifier.py', 'diagnose-highorder-input-reference.py',
    'diagnose-eleventh-order.py', 'diagnose-highorder-amplifier.py',
    'compare-amplifier-terms.py', 'compare-two-tone.py',
    'compare-single-compression.py', 'run-systemvue-reference.py',
    'retired_binding.py', 'reference_records.py', 'native_replay.py',
    'capture-cascade-parameter-units.ps1',
)


def load(filename):
    spec = importlib.util.spec_from_file_location(filename.replace('-', '_'), HERE / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pilot = load('rfamp-parameter-pilot.py')
archive = load('archive-cascade-reference.py')
input_archive = load('archive-cascade-input-reference.py')
measured = load('diagnose-highorder-input-reference.py')
comparison = archive.comparison
helpers = measured.base.helpers


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical_digest(path):
    # Text source identity is portable across Git checkout line endings. Original
    # acquisition bytes have separate exact hashes and embedded source snapshots.
    return digest(Path(path).read_bytes().replace(b'\r\n', b'\n'))


def utc(value):
    parsed = datetime.fromisoformat(value.replace('Z', '+00:00'))
    require(parsed.tzinfo is not None and parsed.utcoffset().total_seconds() == 0,
            'UTC timestamp required')
    return parsed


def encoded(value):
    return [value.real, value.imag]


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write_new(path, value):
    with Path(path).open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, indent=2, allow_nan=False)
        stream.write('\n')


def units_index(supplement, capture):
    require(supplement.get('classification') == 'read_only_parameter_units_supplement_no_analysis',
            'Expected read-only unit supplement')
    require(supplement.get('systemvue_version') == capture['systemvue_version'], 'Unit version mismatch')
    require(utc(supplement['readback']['utc']) >= utc(capture['run_returned_utc']),
            'Unit readback predates completed capture')
    parameters = supplement['readback']['parameters']
    result = {}
    for parameter in parameters:
        path = parameter['path']
        require(path not in result, 'Duplicate parameter unit path')
        fields = {v['name']: v for v in parameter['variables']}
        require(len(fields) == len(parameter['variables']), 'Duplicate unit field')
        result[path] = fields
    require(len(result) == 124, 'Expected complete 124-entry parameter supplement')
    for node in capture['nodes']:
        if node['path'] not in result:
            continue
        fields = result[node['path']]
        require(fields.get('DataEntry', {}).get('value') == node['data_entry'],
                'Unit supplement expression disagrees with capture')
        if 'Data' in fields:
            require(fields['Data']['value'] == node['data'], 'Unit supplement SI value mismatch')
    for part in ('RFAmp1', 'RFAmp2'):
        op = result[comparison.ROOT + 'Design3/PartList/' + part + '/ParamSet/OP1dB']
        require(op['Unit']['type'] == 'Long' and op['Unit']['value'] == 11002,
                'OP1dB original dBm unit ID changed')
    return result


def read_run(directory, run_id, preparation):
    directory = Path(directory)
    manifest = read_json(Path(preparation) / 'manifest.json')
    require(manifest['plan'] == pilot.frozen_plan() and manifest['plan_sha256'] == pilot.PLAN_SHA256,
            'Frozen acquisition plan mismatch')
    require(digest((Path(preparation) / 'plan.json').read_bytes()) == pilot.PLAN_SHA256,
            'Prepared plan bytes changed')
    run = next((run for run in pilot.RUNS if run[0] == run_id), None)
    require(run is not None, 'Unknown pilot run')
    required = ('capture.json', 'status.json', 'stderr.log', 'collector.ps1', 'runner.py',
                'parameter-units.json')
    contents = {name: (directory / name).read_bytes() for name in required}
    raw = json.loads(contents['capture.json'].decode('utf-8-sig'))
    status = json.loads(contents['status.json'].decode('utf-8-sig'))
    pilot.verify_collector_command(status['command'], manifest['windows_root'], run)
    for saved, source in (('collector.ps1', 'capture-cascade-reference.ps1'),
                          ('runner.py', 'run-systemvue-reference.py')):
        require(digest(contents[saved]) == status['source_sha256'][saved] == manifest['source_sha256'][source],
                'Raw acquisition source hash mismatch')
    require(digest(raw['submitted_script'].encode('utf-8')) == status['submitted_script_sha256'],
            'Submitted script binding mismatch')
    require(raw['first_op1db_dbm'] == run[1] and raw['two_tone'] is run[2]
            and raw['source_phase_deg'] == run[3] and raw['source_power_dbm'] == 5
            and raw['second_source_power_dbm'] == 5 and raw['maximum_order'] == 11,
            'Run identity differs from frozen sequence')
    capture = archive.extract(raw, status, digest(contents['capture.json']))
    reference = input_archive.extract(raw, status, digest(contents['capture.json']))
    for key in ('F1', 'P1', 'V1', 'Z1', 'ID1'):
        capture['nodes'].append(comparison.node(raw, comparison.SPECTRUM + key))
    comparison.spectrum(capture, 1)
    measured.measured_sources(capture, reference)
    units = json.loads(contents['parameter-units.json'].decode('utf-8-sig'))
    units_index(units, capture)
    require(units['source_sha256'] == digest(UNIT_SOURCE.read_bytes()), 'Unit helper bytes changed')
    stderr = contents['stderr.log'].decode('utf-8-sig')
    expected_stderr = ('phase: submit-cascade ' + raw['run_started_utc'] + '\n'
                       + 'phase: analysis-returned ' + raw['run_returned_utc'] + '\nmanager-errors: \n')
    require(stderr.replace('\r\n', '\n') == expected_stderr, 'Unexpected collector stderr')
    return {'run_id': run_id, 'capture': capture, 'input_reference': reference,
            'parameter_units': units, 'status': status, 'stderr': stderr,
            'files_sha256': {name: digest(data) for name, data in contents.items()}}


def validate_ab(a, b):
    require(a['run_id'] == 'A-baseline' and b['run_id'] == 'B-op1db21', 'A/B run order mismatch')
    ac, bc = a['capture'], b['capture']
    require(ac['first_op1db_dbm'] == 20 and bc['first_op1db_dbm'] == 21, 'A/B OP1dB mismatch')
    require(utc(ac['run_returned_utc']) < utc(bc['run_started_utc']), 'A/B overlap/order mismatch')
    require(ac['workspace_sha256'] == bc['workspace_sha256'], 'Workspace bytes changed between A/B')
    expected_script = ac['submitted_script'].replace(
        'RFAmp1.ParamSet.OP1dB.Set("20")', 'RFAmp1.ParamSet.OP1dB.Set("21")')
    require(expected_script == bc['submitted_script'] and expected_script != ac['submitted_script'],
            'A/B submitted script changed beyond first OP1dB')
    ai, bi = units_index(a['parameter_units'], ac), units_index(b['parameter_units'], bc)
    require(ai.keys() == bi.keys(), 'A/B parameter identities differ')
    differences = []
    for path, af in ai.items():
        bf = bi[path]
        require(af.keys() == bf.keys(), 'A/B parameter metadata fields differ')
        for name, value in af.items():
            if value != bf[name]:
                differences.append({'path': path, 'field': name, 'A': value, 'B': bf[name]})
    target = comparison.ROOT + 'Design3/PartList/RFAmp1/ParamSet/OP1dB'
    require({(d['path'], d['field']) for d in differences} == {(target, 'DataEntry'), (target, 'Data')},
            'A/B parameter readback differs beyond first OP1dB')
    require(comparison.node(ac, 'System3')['analysis_settings'] == comparison.node(bc, 'System3')['analysis_settings'],
            'A/B analysis settings differ')
    return differences


def point_for(native, capture, sources):
    return native.amplifier_operating_point(
        math.fsum(abs(source.amplitude)**2 for source in sources), power_gain_db=10,
        output_p1db_dbm=capture['first_op1db_dbm'], output_saturation_dbm=23,
        input_ip2_dbm=30, input_ip3_dbm=20, reference_ohms=50)


def identify(native, packet):
    capture = packet['capture']
    require(not capture['two_tone'], 'Identification is restricted to single-tone A/B')
    sources, input_info = measured.measured_sources(capture, packet['input_reference'])
    point = point_for(native, capture, sources)
    wave = sources[0].amplitude
    limited = wave * point.nonlinear_input_scale
    observed = comparison.spectrum(capture, 3)
    coefficients, fits = [0.0] * 12, []
    for order in ORDERS:
        label = comparison.label(comparison.traced.expression((10,) * order), 1)
        require(label in observed, 'Missing isolated H' + str(order) + ' identification term')
        rows = helpers.pair(observed[label], order * 1e9, float(order))
        ratios = [row[1] / (5.0**(order - 1) * limited**order) for row in rows]
        require(all(math.isfinite(v.real) and math.isfinite(v.imag) and v.real < 0
                    and abs(v.imag) <= TOLERANCE * abs(v) for v in ratios),
                'Calibration is not a finite negative real coefficient')
        require(abs(ratios[0] / ratios[1] - 1) <= TOLERANCE, 'Calibration band edges disagree')
        coefficient = math.fsum(v.real for v in ratios) / 2
        coefficients[order] = coefficient
        predicted = coefficient * 5.0**(order - 1) * limited**order
        fits.append({'order': order, 'label': label, 'voltage_coefficient': coefficient,
                     'edge_coefficients': [encoded(v) for v in ratios],
                     'observed_waves': [encoded(row[1]) for row in rows],
                     'reconstruction_relative_error': max(abs(row[1] / predicted - 1) for row in rows)})
    direct = comparison.label('(Source.Source1)', 1, direct=True)
    require(direct in observed, 'Missing direct fundamental common-response control')
    direct_rows = helpers.pair(observed[direct], 1e9, 1.)
    direct_expected = wave * point.fundamental_amplitude_gain
    error = max(abs(row[1] / direct_expected - 1) for row in direct_rows)
    return {'run_id': packet['run_id'], 'coefficients_c0_to_c11': coefficients,
            'measured_input': input_info, 'operating_point': point._asdict(), 'isolated_harmonics': fits,
            'common_fundamental_control': {'expected_wave': encoded(direct_expected),
                'observed_waves': [encoded(row[1]) for row in direct_rows],
                'relative_wave_error': error, 'passed': error <= TOLERANCE},
            'independent_prediction_count': 0}


def predict(native, capture, sources, coefficients):
    require(len(coefficients) == 12 and all(math.isfinite(v) for v in coefficients), 'Invalid frozen coefficients')
    require(all(coefficients[n] < 0 if n in ORDERS else coefficients[n] == 0 for n in range(12)),
            'Only registered odd high-order coefficients are predicted')
    point = point_for(native, capture, sources)
    scaled = [source._replace(amplitude=source.amplitude * point.nonlinear_input_scale) for source in sources]
    response = native.coherent_polynomial(1e8, scaled, coefficients, reference_ohms=50)
    result, seen = [], set()
    for term in response.terms:
        label = measured.base.term_label(term, response.inputs)
        require(label not in seen, 'Duplicate predicted origin')
        seen.add(label)
        result.append({'order': term.order, 'label': label, 'bin': term.component.bin,
                       'bandwidth_hz': term.component.bandwidth_hz,
                       'expected_wave': encoded(term.component.amplitude)})
    return {'operating_point': point._asdict(), 'terms': result}


def compare_terms(prediction, observed):
    checks, missing, labels = [], [], set()
    for term in prediction['terms']:
        label = term['label']
        require(label not in labels, 'Duplicate frozen prediction identity')
        labels.add(label)
        if label not in observed:
            missing.append(term)
            continue
        rows = helpers.pair(observed[label], term['bin'] * 1e8, term['bandwidth_hz'])
        expected = complex(*term['expected_wave'])
        require(expected != 0, 'Zero prediction cannot be a relative-wave reference')
        error = max(abs(row[1] / expected - 1) for row in rows)
        checks.append({**term, 'observed_waves': [encoded(row[1]) for row in rows],
                       'relative_wave_error': error, 'passed': error <= TOLERANCE})
    unexpected = []
    for label in observed:
        if label.startswith('[') and label.endswith('],RFAmp1'):
            counts = re.findall(r'(?:(\d+)x)?\(Source\.Source[12]\)', label)
            if sum(int(c) if c else 1 for c in counts) in ORDERS and label not in labels:
                unexpected.append(label)
    return {'expected_origin_count': len(labels), 'recorded_origin_count': len(checks),
            'passed_origin_count': sum(c['passed'] for c in checks),
            'failed_origin_count': sum(not c['passed'] for c in checks),
            'max_relative_wave_error': max((c['relative_wave_error'] for c in checks), default=None),
            'missing_origins': missing, 'unexpected_origins': unexpected,
            'complete_and_passed': not missing and not unexpected and bool(checks) and all(c['passed'] for c in checks),
            'checks': checks}


def current_logic():
    return {name: canonical_digest(HERE / name) for name in LOGIC_SOURCES}


def freeze(native, a, b, acquisition_root):
    acquisition_root = Path(acquisition_root)
    require(not (acquisition_root / 'C-two-tone-holdout').exists()
            and not (acquisition_root / 'D-restore-baseline').exists(), 'C/D already acquired; cannot preregister')
    differences = validate_ab(a, b)
    fit_a, fit_b = identify(native, a), identify(native, b)
    bsources, _ = measured.measured_sources(b['capture'], b['input_reference'])
    null_prediction = predict(native, b['capture'], bsources, fit_a['coefficients_c0_to_c11'])
    null_result = compare_terms(null_prediction, comparison.spectrum(b['capture'], 3))
    # The C numerical preview transfers B's measured voltage to the second source
    # and rotates it by 90 degrees. This is a declared prospective input assumption,
    # not a claim that the as-yet-unmeasured C input is known. Final primary scoring
    # applies this same frozen predictor to measured C V1, without any refitting.
    wave = bsources[0].amplitude
    prospective = [bsources[0], CoherentComponent(11, SpectrumKind.SOURCE, 1., 2, 1j * wave)]
    prediction = predict(native, b['capture'], prospective, fit_b['coefficients_c0_to_c11'])
    ratios = {str(order): fit_b['coefficients_c0_to_c11'][order] / fit_a['coefficients_c0_to_c11'][order]
              for order in ORDERS}
    return {'schema_version': 1, 'classification': 'frozen_before_C_not_compatibility',
            'created_utc': datetime.now(timezone.utc).isoformat(),
            'plan_sha256': pilot.PLAN_SHA256, 'relative_complex_wave_tolerance': TOLERANCE,
            'workspace_sha256': a['capture']['workspace_sha256'],
            'scope': 'First-stage independent generated odd origins of orders 5,7,9,11 only',
            'normalization': {'input': 'measured complex V1_peak/sqrt(2*50 ohm)',
                              'output': 'measured complex V3_peak/sqrt(2*Z3)',
                              'drive': 'sum(abs(measured input wave)^2)',
                              'coefficient': 'Hn/(5^(n-1)*(measured input wave*common nonlinear scale)^n)',
                              'input_for_C_primary_score': 'C measured input only; frozen coefficients and predictor'},
            'A_B_parameter_differences': differences, 'A': fit_a, 'B': fit_b,
            'B_over_A_coefficients': ratios, 'unchanged_coefficients_at_B': null_result,
            'C_prospective_input_assumption': [
                {'bin': s.bin, 'wave': encoded(s.amplitude), 'bandwidth_hz': s.bandwidth_hz,
                 'coherence_group': s.coherence_group} for s in prospective],
            'C_prospective_prediction': prediction,
            'logic_sha256_lf': current_logic(),
            'calibration_capture_sha256': {p['run_id']: p['capture']['raw_capture_sha256'] for p in (a, b)},
            'limits': ['A and B are identification/control data, never independent holdout points.',
                       'C inputs are unmeasured at freeze; the numeric preview makes an explicit B-to-C input transfer assumption.',
                       'The primary C comparison uses measured C input with the unchanged frozen conditional predictor.',
                       'Common-response and coefficient-model errors remain separate observable residuals.',
                       'Missing terms remain missing; no refitting, zero filling or tolerance widening.',
                       'This does not identify a proprietary coefficient law or certify full RFAMP/network compatibility.']}


def score_c(native, frozen, packet, *, require_frozen_runtime=False):
    require(frozen['classification'] == 'frozen_before_C_not_compatibility'
            and frozen['plan_sha256'] == pilot.PLAN_SHA256
            and frozen['relative_complex_wave_tolerance'] == TOLERANCE, 'Frozen contract changed')
    require(frozen['logic_sha256_lf'] == current_logic(), 'Frozen predictor source changed')
    if require_frozen_runtime:
        require(native.execution_identity == frozen['native_replay']['native_replay_execution'],
                'Original holdout scoring requires the frozen native executable and manifest')
    capture = packet['capture']
    require(packet['run_id'] == 'C-two-tone-holdout' and capture['first_op1db_dbm'] == 21
            and capture['two_tone'] and capture['source_phase_deg'] == 90, 'Not the registered C holdout')
    require(utc(capture['run_started_utc']) > utc(frozen['created_utc']), 'C predates prediction freeze')
    require(capture['workspace_sha256'] == frozen['workspace_sha256'], 'C workspace differs from A/B')
    comparison.inspect(capture, allow_parameter_pilot=True)
    sources, input_info = measured.measured_sources(capture, packet['input_reference'])
    prediction = predict(native, capture, sources, frozen['B']['coefficients_c0_to_c11'])
    observed = comparison.spectrum(capture, 3)
    result = compare_terms(prediction, observed)
    preview = compare_terms(frozen['C_prospective_prediction'], observed)
    input_differences = []
    for actual, prospective in zip(sources, frozen['C_prospective_input_assumption']):
        expected = complex(*prospective['wave'])
        input_differences.append({'bin': actual.bin, 'measured_wave': encoded(actual.amplitude),
                                  'prospective_wave': encoded(expected),
                                  'relative_complex_difference': abs(actual.amplitude / expected - 1)})
    return {'classification': 'independent_C_holdout_diagnostic_not_full_compatibility',
            'relative_complex_wave_tolerance': TOLERANCE, 'raw_capture_sha256': capture['raw_capture_sha256'],
            'input_reference': input_info, 'input_preview_differences': input_differences,
            'operating_point': prediction['operating_point'], 'measured_input_prediction': result,
            'prospective_numeric_prediction': preview, 'coefficient_refit_on_C': False}


def score_d(a, c, d):
    """End control only. It never contributes an independent prediction point."""
    ac, cc, dc = (p['capture'] for p in (a, c, d))
    require(d['run_id'] == 'D-restore-baseline' and dc['first_op1db_dbm'] == 20
            and not dc['two_tone'] and dc['source_phase_deg'] == 0, 'Not the registered D end control')
    require(utc(dc['run_started_utc']) > utc(cc['run_returned_utc']), 'D precedes/overlaps C')
    require(ac['workspace_sha256'] == cc['workspace_sha256'] == dc['workspace_sha256'],
            'Workspace bytes changed during pilot')
    require(ac['submitted_script'] == dc['submitted_script'], 'D control commands differ from A')
    require(units_index(a['parameter_units'], ac) == units_index(d['parameter_units'], dc),
            'D control parameters or units differ from A')
    require(comparison.node(ac, 'System3')['analysis_settings'] == comparison.node(dc, 'System3')['analysis_settings'],
            'D analysis settings differ from A')
    ports = {}
    for port in (1, 3, 2):
        first, last = comparison.spectrum(ac, port), comparison.spectrum(dc, port)
        missing, extra = sorted(set(first) - set(last)), sorted(set(last) - set(first))
        errors, exact, groups_exact = [], True, True
        for label in sorted(set(first) & set(last)):
            left, right = sorted(first[label]), sorted(last[label])
            require(len(left) == len(right) and [r[0] for r in left] == [r[0] for r in right],
                    'A/D control spectrum support differs')
            for x, y in zip(left, right):
                errors.append(abs(y[1] / x[1] - 1))
                exact = exact and y[1] == x[1]
                groups_exact = groups_exact and y[2] == x[2]
        ports[str(port)] = {'origin_count_A': len(first), 'origin_count_D': len(last),
                            'compared_band_edge_rows': len(errors), 'missing': missing, 'extra': extra,
                            'complex_waves_exact': exact, 'coherence_ids_exact': groups_exact,
                            'max_relative_wave_error': max(errors, default=None),
                            'passed': not missing and not extra and bool(errors) and max(errors) <= TOLERANCE}
    return {'classification': 'D_end_control_not_new_prediction', 'independent_prediction_count': 0,
            'relative_complex_wave_tolerance': TOLERANCE, 'ports': ports,
            'all_controls_passed': all(p['passed'] for p in ports.values())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-probe', required=True, type=Path)
    sub = parser.add_subparsers(dest='mode', required=True)
    freezing = sub.add_parser('freeze')
    freezing.add_argument('--preparation', required=True, type=Path)
    freezing.add_argument('--runs', required=True, type=Path)
    freezing.add_argument('--ab-output', required=True, type=Path)
    freezing.add_argument('--output', required=True, type=Path)
    scoring = sub.add_parser('score-c')
    scoring.add_argument('--preparation', required=True, type=Path)
    scoring.add_argument('--runs', required=True, type=Path)
    scoring.add_argument('--freeze', required=True, type=Path)
    scoring.add_argument('--freeze-sha256', required=True)
    scoring.add_argument('--c-output', required=True, type=Path)
    scoring.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    native = NativeReplay(args.native_probe)
    if args.mode == 'freeze':
        require(not args.output.exists() and not args.ab_output.exists(), 'Freeze output already exists')
        require(not (args.runs / 'C-two-tone-holdout').exists() and not (args.runs / 'D-restore-baseline').exists(),
                'C/D already acquired; cannot preregister')
        a = read_run(args.runs / 'A-baseline', 'A-baseline', args.preparation)
        b = read_run(args.runs / 'B-op1db21', 'B-op1db21', args.preparation)
        result = freeze(native, a, b, args.runs)
        ab = {'schema_version': 1, 'classification': 'A_B_identification_only', 'runs': [a, b]}
        write_new(args.ab_output, ab)
        result['ab_archive_sha256'] = digest(args.ab_output.read_bytes())
        result['native_replay'] = native.manifest([args.ab_output], [HERE / n for n in LOGIC_SOURCES])
        write_new(args.output, result)
        print(json.dumps({'freeze_sha256': digest(args.output.read_bytes()),
                          'B_over_A_coefficients': result['B_over_A_coefficients'],
                          'null_at_B_complete_and_passed': result['unchanged_coefficients_at_B']['complete_and_passed'],
                          'C_predicted_origins': len(result['C_prospective_prediction']['terms'])}, indent=2))
    else:
        require(digest(args.freeze.read_bytes()) == args.freeze_sha256, 'Frozen artifact bytes changed')
        require(not args.output.exists() and not args.c_output.exists(), 'Score output already exists')
        frozen = read_json(args.freeze)
        c = read_run(args.runs / 'C-two-tone-holdout', 'C-two-tone-holdout', args.preparation)
        result = score_c(native, frozen, c, require_frozen_runtime=True)
        write_new(args.c_output, {'schema_version': 1, 'classification': 'independent_C_acquisition', 'run': c})
        result['freeze_sha256'] = args.freeze_sha256
        result['c_archive_sha256'] = digest(args.c_output.read_bytes())
        result['native_replay'] = native.manifest([args.freeze, args.c_output], [HERE / n for n in LOGIC_SOURCES])
        write_new(args.output, result)
        print(json.dumps({key: value for key, value in result['measured_input_prediction'].items()
                          if key not in ('checks', 'missing_origins', 'unexpected_origins')}, indent=2))


if __name__ == '__main__':
    main()

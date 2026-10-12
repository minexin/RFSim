"""Compare existing A01 evidence to the actual C API; never calibrate the kernel."""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'python'))
from rfmodel import Library
spec = importlib.util.spec_from_file_location('archive', Path(__file__).with_name('archive-am-pm-datapoints.py'))
archive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(archive)
hyp = archive.hypotheses
TOLERANCE = 1e-5


def positive(value):
    value = hyp.number(value)
    if value <= 0:
        raise ValueError('Positive power required')
    return value


def validate_capture(capture, expected_path):
    if capture['dataset_path'] != expected_path:
        raise ValueError('Wrong analysis path')
    if not re.fullmatch(r'[0-9a-f]{64}', capture['source']['sha256']):
        raise ValueError('Missing capture digest')
    start = archive.parse_utc(capture['run_started_utc'])
    end = archive.parse_utc(capture['run_returned_utc'])
    stamp = float(capture['dataset_timestamp'])
    if not start <= end or not math.floor(start) <= stamp <= math.ceil(end):
        raise ValueError('Dataset is not fresh for this run')
    points = capture['points']
    if len(points) != 37 or any(type(p['index']) is not int or p['index'] != i for i, p in enumerate(points)):
        raise ValueError('Expected all 37 ordered A01 points')


def summary(rows):
    return dict(point_count=len(rows), passed_count=sum(r['passed'] for r in rows),
                passed=bool(rows) and all(r['passed'] for r in rows),
                max_circular_error_deg=max(r['circular_error_deg'] for r in rows), rows=rows)


def analyze(evidence, library):
    if evidence['schema_version'] != 1 or evidence['new_systemvue_analysis_calls'] != 0:
        raise ValueError('Expected offline A01 extraction')
    if hyp.number(evidence['phase_absolute_tolerance_degrees']) != TOLERANCE:
        raise ValueError('The recorded tolerance must not change')
    pd = evidence['point_derivative']
    validate_capture(pd, 'RFModel_AMPM_A01/PointDerivative/System1_Data_Folder/System1_Data_Path2')
    expected = dict(AMtoPM_Mode=1, AMtoPM=-5, PwrAMtoPM=.01, G=100, OP1dB=.01, OPSAT=.0199526231496888)
    if any(hyp.number(pd['parameters_si'][k]) != v for k, v in expected.items()):
        raise ValueError('Unexpected PointDerivative SI parameter readback')
    if hyp.number(pd['peak_degrees_per_db']) != -5 or hyp.number(pd['output_reference_dbm']) != 10:
        raise ValueError('PointDerivative parameter units do not match readback')
    rows = []
    for point in pd['points']:
        power = positive(point['output_power_w'])
        source = hyp.number(point['input_phase_rad'])
        actual = math.degrees(hyp.number(point['phase_rad']) - source)
        response = library.am_pm_response(power, peak_degrees_per_db=-5, output_reference_dbm=10)
        predicted = math.degrees(hyp.number(response.phase_rad))
        error = hyp.circular_error(actual, predicted)
        rows.append(dict(index=point['index'], output_dbm=10 * math.log10(power) + 30,
                         systemvue_phase_deg=actual, rfmodel_phase_deg=predicted,
                         circular_error_deg=error, passed=error <= TOLERANCE))
    # Retrospective two-point fit: never changes the native verdict.
    u = [math.sqrt(p['output_power_w'] / .01) for p in pd['points']]
    if max(u) >= 1.5:
        raise ValueError('This exploratory cubic fit only covers the unclamped branch')
    i, j = 7, 17
    y = [r['systemvue_phase_deg'] for r in rows]
    b = (y[j] / u[j]**2 - y[i] / u[i]**2) / (u[j] - u[i])
    a = y[i] / u[i]**2 - b * u[i]
    residuals = [hyp.circular_error(y[n], a * u[n]**2 + b * u[n]**3) for n in range(len(u)) if n not in (i, j)]
    peak_u = -4 * a / (9 * b)
    fit = dict(classification='retrospective_fit_not_independent_validation', fitted_indices=[i, j],
               remaining_point_count=len(residuals), amplitude_squared_coefficient_deg=a,
               amplitude_cubed_coefficient_deg=b, remaining_max_circular_error_deg=max(residuals),
               inferred_peak_output_shift_db=20 * math.log10(peak_u),
               inferred_peak_degrees_per_db=math.log(10)/20 * (2*a*peak_u**2+3*b*peak_u**3),
               changes_native_verdict=False)
    quick_rows, names = [], set()
    if len(evidence['quick_sweeps']) != 4:
        raise ValueError('Expected the four selected A01 scans')
    for scan in evidence['quick_sweeps']:
        validate_capture(scan, 'RFModel_AMPM_A01/DataPoints/System2_Data_Folder/System2_Sch2_Data_Path2')
        profile = scan['profile']
        name = profile['name']
        if name in names:
            raise ValueError('Duplicate scan profile')
        names.add(name)
        powers, phases = hyp.validate_table(profile['power_dbm'], profile['phase_deg'])
        normal_phase = hyp.number(scan['normal_source_phase_deg'])
        if hyp.circular_error(normal_phase, profile['source_phase_deg']) > TOLERANCE:
            raise ValueError('Normal source phase disagrees with profile')
        if len(scan['sweep_source_phase_deg']) != len(scan['points']):
            raise ValueError('Incomplete quick-sweep source phases')
        for point, saved_source in zip(scan['points'], scan['sweep_source_phase_deg']):
            power = positive(point['reported_output_w'])
            for key in ('reported_input_w', 'swept_input_w', 'gain'):
                positive(point[key])
            source = math.degrees(hyp.number(point['input_phase_rad']))
            if hyp.circular_error(source, saved_source) > TOLERANCE:
                raise ValueError('Quick-sweep source phase readbacks disagree')
            actual = math.degrees(hyp.number(point['output_phase_rad']))
            dbm = 10 * math.log10(power) + 30
            predicted = hyp.natural_cubic(powers, phases, dbm) + source
            error = hyp.circular_error(actual, predicted)
            quick_rows.append(dict(profile=name, index=point['index'], output_dbm=dbm,
                                   normal_source_phase_deg=normal_phase, sweep_source_phase_deg=source,
                                   systemvue_phase_deg=actual, candidate_phase_deg=predicted,
                                   circular_error_deg=error, passed=error <= TOLERANCE))
    return dict(schema_version=1, new_systemvue_analysis_calls=0, phase_absolute_tolerance_degrees=TOLERANCE,
                point_derivative_native=summary(rows), exploratory_cubic_fit=fit,
                quick_sweep_natural_cubic_candidate=summary(quick_rows),
                quick_sweep_classification='retrospective_candidate_not_native_implementation',
                eligible_for_full_model_compatibility=False)


def verify_raw_sources(evidence, root):
    """Optional local replay; normalized evidence alone does not imply raw verification."""
    for position, capture in enumerate([evidence['point_derivative']] + evidence['quick_sweeps']):
        path = root / capture['source']['path']
        if archive.digest(path) != capture['source']['sha256']:
            raise ValueError('Raw capture hash changed')
        raw = archive.load(path)
        nodes = {}
        for node in raw['nodes']:
            nodes.setdefault(node['path'], []).append(node)

        def get(path):
            if len(nodes.get(path, [])) != 1:
                raise ValueError('Missing or duplicate raw node: ' + path)
            return nodes[path][0]

        for field in ('run_started_utc', 'run_returned_utc'):
            if raw[field] != capture[field]:
                raise ValueError('Run timestamp changed')
        if get(capture['dataset_path'])['timestamp'] != capture['dataset_timestamp']:
            raise ValueError('Dataset timestamp changed')
        base = capture['dataset_path'] + '/Eqns/VarBlock/'

        def values(name):
            node = get(base + name)
            if node['dimensions'] != [74] or len(node['data']) != 74:
                raise ValueError('Wrong sweep dimensions')
            return node['data']

        if values('NodeNames') != ['1', '2'] * 37 or values('CF') != [1e8] * 74:
            raise ValueError('Wrong sweep node or frequency')
        power, phase = values('DCP'), values('DCPH')
        points = []
        if position == 0:
            amp = 'RFModel_AMPM_A01/PointDerivative/Sch1/PartList/RFAmp/ParamSet/'
            if any(get(amp + k)['data'] != v for k, v in capture['parameters_si'].items()):
                raise ValueError('PointDerivative parameter extraction changed')
            for i in range(37):
                points.append(dict(index=i, output_power_w=power[2*i+1],
                                   phase_rad=phase[2*i+1], input_phase_rad=phase[2*i]))
        else:
            normal = archive.normalize(raw, capture['profile'])
            if normal['measured_source_phase_deg'] != capture['normal_source_phase_deg']:
                raise ValueError('Normal source phase extraction changed')
            sweep, gain = values('SwpPwr_Swp_NodeNames'), values('CGAIN')
            for i in range(37):
                points.append(dict(index=i, swept_input_w=sweep[2*i], reported_input_w=power[2*i],
                                   reported_output_w=power[2*i+1], input_phase_rad=phase[2*i],
                                   output_phase_rad=phase[2*i+1], gain=gain[2*i+1]))
        if points != capture['points']:
            raise ValueError('Normalized points differ from raw capture')
    return True

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence', type=Path)
    parser.add_argument('library', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--raw-root', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('Output already exists; evidence will not be overwritten')
    evidence = archive.load(args.evidence)
    raw_verified = verify_raw_sources(evidence, args.raw_root) if args.raw_root else False
    result = analyze(evidence, Library(args.library.resolve()))
    result['raw_sources_verified'] = raw_verified
    result['evidence_sha256'] = archive.digest(args.evidence)
    result['native_library_sha256'] = archive.digest(args.library)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write('\n')
    return 0 if result['point_derivative_native']['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())

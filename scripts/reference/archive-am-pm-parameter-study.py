"""Validate and normalize the preregistered PointDerivative captures, without COM."""
import argparse
import importlib.util
import json
import math
from pathlib import Path

spec = importlib.util.spec_from_file_location('archive', Path(__file__).with_name('archive-am-pm-datapoints.py'))
archive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(archive)
BASE = 'RFModel_AMPM_A01/PointDerivative/'
AMP = BASE + 'Sch1/PartList/RFAmp/ParamSet/'
SOURCE = BASE + 'Sch1/PartList/Source/ParamSet/'
PATH = BASE + 'System1_Data_Folder/System1_Data_Path'
RAW = BASE + 'System1_Data'
PLAN_SHA = '8ab865ee8e8300e9201e3f111d0bfb674da98f51a6988de60920b4bbdddd5650'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value):
    require(type(value) in (int, float) and math.isfinite(value), 'Finite number required')
    return float(value)


def expected_parameters(profile):
    sweep = BASE + 'System1/Path1/'
    gain = profile.get('gain_db', 20)
    frequency = profile.get('frequency_hz', 1e8)
    return {
        AMP + 'AMtoPM_Mode': 1, AMP + 'AMtoPM': profile['peak_degrees_per_db'],
        AMP + 'PwrAMtoPM': 10**((profile['reference_dbm']-30)/10), AMP + 'G': 10**(gain/10),
        AMP + 'OP1dB': 10**((profile.get('output_p1db_dbm', 100)-30)/10),
        AMP + 'OPSAT': 10**((profile.get('output_saturation_dbm', 103)-30)/10),
        AMP + 'OIP2': 10**((profile.get('output_ip2_dbm', 140)-30)/10),
        AMP + 'OIP3': 10**((profile.get('output_ip3_dbm', 130)-30)/10),
        SOURCE + 'Pwr': 10**((profile['ordinary_output_target_dbm']-gain-30)/10),
        SOURCE + 'Phase': math.radians(profile.get('source_phase_deg', 30)), SOURCE + 'Freq': frequency,
        sweep + 'StartPwr': 10**((profile['sweep_input_start_dbm']-30)/10),
        sweep + 'StopPwr': 10**((profile['sweep_input_stop_dbm']-30)/10),
        sweep + 'StepPwr': 10**(profile.get('sweep_step_db', .25)/10), sweep + 'AutoLimits': 0,
        sweep + 'Freq': frequency, sweep + 'SweepType': 3,
    }


def normalize(capture, profile, *, allow_diagnostic=False, retain_extra_endpoint=False):
    require(capture['profile'] == profile['name'], 'Wrong profile')
    require(capture['analysis_done'] == 1, 'Incomplete analysis')
    diagnostics = capture['manager_errors'].strip()
    require(allow_diagnostic or not diagnostics, 'Diagnostic run')
    frequency = profile.get('frequency_hz', 1e8)
    gain = profile.get('gain_db', 20)
    source_phase = math.radians(profile.get('source_phase_deg', 30))
    expected_count = profile.get('expected_points', 73)
    step = profile.get('sweep_step_db', .25)
    start = archive.parse_utc(capture['run_started_utc'])
    end = archive.parse_utc(capture['run_returned_utc'])
    require(start <= end, 'Reversed run times')
    nodes = {}
    for n in capture['nodes']:
        nodes.setdefault(n['path'], []).append(n)

    def node(path):
        require(len(nodes.get(path, [])) == 1, 'Missing or duplicate node: ' + path)
        return nodes[path][0]

    def data(path):
        return node(path)['data']

    def vector(path, length=None):
        n = node(path)
        value = n['data']
        require(isinstance(value, list) and n['dimensions'] == [len(value)], 'Invalid vector shape: ' + path)
        require(length is None or len(value) == length, 'Invalid vector length: ' + path)
        return value

    timestamps = {p: int(node(p)['timestamp']) for p in (PATH + '1', PATH + '2', RAW)}
    require(all(math.floor(start) <= t <= math.ceil(end) for t in timestamps.values()), 'Stale dataset')
    expected = expected_parameters(profile)
    readbacks = {}
    for key, value in expected.items():
        actual = number(data(key))
        require(abs(actual-value) <= 1e-12*max(1e-12, abs(value)), 'Parameter mismatch: ' + key)
        readbacks[key] = actual

    def points(path, count):
        key = path + '/Eqns/VarBlock/'
        require(vector(key + 'NodeNames', 2*count) == ['1', '2']*count, 'Wrong nodes')
        require(vector(key + 'CF', 2*count) == [frequency]* (2*count), 'Wrong channel frequency')
        power = [number(v) for v in vector(key + 'DCP', 2*count)]
        phase = [number(v) for v in vector(key + 'DCPH', 2*count)]
        require(all(v > 0 for v in power), 'Nonpositive channel power')
        return [dict(index=i, input_power_w=power[2*i], output_power_w=power[2*i+1],
                     input_phase_rad=phase[2*i], output_phase_rad=phase[2*i+1]) for i in range(count)]

    ordinary = points(PATH + '1', 1)[0]
    require(abs(ordinary['input_phase_rad']-source_phase) < 1e-10, 'Ordinary phase mismatch')
    require(abs(10*math.log10(ordinary['input_power_w'])+30 - (profile['ordinary_output_target_dbm']-gain)) < 1e-5,
            'Ordinary input power mismatch')
    count = expected_count
    if retain_extra_endpoint:
        size = len(vector(PATH + '2/Eqns/VarBlock/DCP'))
        require(size in (2*expected_count, 2*(expected_count+1)), 'Unexpected sweep size')
        count = size//2
    rows = points(PATH + '2', count)
    for i, row in enumerate(rows):
        nominal = profile['sweep_input_start_dbm'] + step*i
        require(abs(10*math.log10(row['input_power_w'])+30-nominal) < 1e-5, 'Sweep grid mismatch')
        require(abs(row['input_phase_rad']) < 1e-10, 'Unexpected sweep source phase')
        row['nominal_output_dbm'] = nominal + gain

    key = RAW + '/Eqns/VarBlock/'
    names = vector(key + 'IDName')
    ids = vector(key + 'IDNo', len(names))
    require(len(set(ids)) == len(ids), 'Duplicate signal IDs')
    mapping = dict(zip(ids, names))
    desired = [i for i, name in mapping.items() if name.startswith('{1}D[') and name.endswith(',Source,RFAmp')]
    require(len(desired) == 1, 'Ambiguous desired signal')
    frequencies = vector(key + 'F2')
    count = len(frequencies)
    labels = vector(key + 'ID2', count)
    powers = vector(key + 'P2', count)
    volts = vector(key + 'V2', 2*count)
    impedances = vector(key + 'Z2', 2*count)
    carrier = []
    for i, label in enumerate(labels):
        if label != desired[0]:
            continue
        v = complex(*map(number, volts[2*i:2*i+2]))
        z = complex(*map(number, impedances[2*i:2*i+2]))
        p = number(powers[i])
        require(z != 0 and p > 0 and archive.relative(.5*abs(v)**2*(1/z).real, p) < 1e-12,
                'Raw V/Z/P inconsistency')
        phase = math.degrees(math.atan2(v.imag, v.real))
        require(archive.hypotheses.circular_error(phase, math.degrees(ordinary['output_phase_rad'])) < 1e-8,
                'Raw versus ordinary phase mismatch')
        carrier.append(dict(frequency_hz=number(frequencies[i]), power_w=p, voltage=[v.real, v.imag],
                            impedance=[z.real, z.imag], output_phase_deg=phase, label=mapping[label]))
    require(len(carrier) == 2 and sorted(c['frequency_hz'] for c in carrier) == [frequency-.5, frequency+.5],
            'Unexpected raw carrier support')
    require(archive.relative(carrier[0]['power_w'], carrier[1]['power_w']) < 1e-12, 'Nonconstant raw spectrum')
    ordinary['raw_carrier'] = carrier
    ordinary['raw_output_w'] = sum(c['power_w'] for c in carrier)/2
    ordinary['raw_vs_report_relative_error'] = archive.relative(ordinary['raw_output_w'], ordinary['output_power_w'])
    result = dict(profile={k: v for k, v in profile.items() if k != 'predictions'},
                run_started_utc=capture['run_started_utc'], run_returned_utc=capture['run_returned_utc'],
                timestamps=timestamps, parameters_si=readbacks, ordinary=ordinary, sweep=rows[:expected_count])
    if retain_extra_endpoint:
        result['additional_sweep_points'] = rows[expected_count:]
    if allow_diagnostic:
        result['diagnostics'] = diagnostics
        result['warning_free_acceptance_eligible'] = not diagnostics
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('root', type=Path)
    p.add_argument('plan', type=Path)
    p.add_argument('manifest', type=Path)
    p.add_argument('output', type=Path)
    args = p.parse_args()
    require(not args.output.exists(), 'Output already exists')
    require(archive.digest(args.plan) == PLAN_SHA, 'Registered plan changed')
    plan, manifest = archive.load(args.plan), archive.load(args.manifest)
    require(manifest['plan_sha256'] == PLAN_SHA, 'Manifest plan mismatch')
    ledger = manifest['actual_analysis_ledger']
    require(len(ledger) == 8 and sum(row['call_count'] for row in ledger) == 8,
            'Analysis budget ledger changed')
    require(len({row['directory'] for row in ledger}) == 8, 'Repeated analysis ledger entry')
    for row in ledger:
        directory = args.root / row['directory']
        for filename, field in [('status.json', 'status_sha256'), ('stdout.log', 'stdout_sha256')]:
            require(archive.digest(directory / filename) == row[field], 'Analysis ledger bytes changed')
    prepared = {p['name']: p for p in manifest['prepared_workspaces']['profiles']}
    sources = manifest['captures']
    require([s['profile'] for s in sources] == [p['name'] for p in plan['profiles']], 'Incomplete or unordered profiles')
    samples = []
    for profile, source in zip(plan['profiles'], sources):
        path = args.root / source['path']
        require(archive.digest(path) == source['sha256'], 'Raw capture changed')
        for filename, field in [('status.json', 'status_sha256'), ('before.json', 'before_sha256'),
                                ('closed.json', 'closed_sha256'), ('RFModel_AMPM_A01-result.wsv', 'result_workspace_sha256')]:
            require(archive.digest(path.parent / filename) == source[field], 'Capture artifact changed')
        status = archive.load(path.parent / 'status.json')
        closed = archive.load(path.parent / 'closed.json')
        require(status['exit_code'] == 0 and status['profile'] == profile['name'], 'Capture helper failed')
        require(closed['workspace_count'] == 0, 'Result workspace not closed')
        require(closed['input_sha256'] == prepared[profile['name']]['workspace_sha256'], 'Prepared input overwritten')
        row = normalize(archive.load(path), profile)
        row['capture_source'] = source
        samples.append(row)
    result = dict(schema_version=1, classification='preregistered_parameter_mapping_transfer_study',
                  plan_sha256=PLAN_SHA, manifest_sha256=archive.digest(args.manifest),
                  phase_absolute_tolerance_degrees=plan['phase_absolute_tolerance_degrees'],
                  raw_capture_bytes_verified=True, new_systemvue_analysis_calls=0,
                  hypotheses=plan['hypotheses'], samples=samples, eligible_for_full_model_compatibility=False)
    with args.output.open('x', encoding='utf-8') as f:
        json.dump(result, f, indent=2, allow_nan=False)
        f.write('\n')
    print('Validated six profiles, 438 swept points and six ordinary/raw observations')


if __name__ == '__main__':
    main()

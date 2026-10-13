"""Validate one preregistered DataPoints capture without accessing SystemVue."""

import argparse
import copy
from decimal import Decimal
import importlib.util
import json
import math
from pathlib import Path
import re


_spec = importlib.util.spec_from_file_location(
    'datapoints_archive', Path(__file__).with_name('archive-am-pm-datapoints.py')
)
archive = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(archive)

BASE = 'RFModel_AMPM_A01/DataPoints/'
ANALYSIS = BASE + 'System2'
AMP = BASE + 'Sch2/PartList/RFAmp/ParamSet/'
SOURCE = BASE + 'Sch2/PartList/Source/ParamSet/'
SWEEP = ANALYSIS + '/Path1/'
PATH = BASE + 'System2_Data_Folder/System2_Sch2_Data_Path'
RAW = BASE + 'System2_Sch2_Data'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value):
    require(type(value) in (int, float), 'Finite number required')
    try:
        finite = math.isfinite(value)
    except OverflowError as error:
        raise ValueError('Finite number required') from error
    require(finite, 'Finite number required')
    return float(value)


def _power(dbm):
    try:
        result = 10 ** ((number(dbm) - 30) / 10)
    except OverflowError as error:
        raise ValueError('Unrepresentable power') from error
    require(math.isfinite(result) and result > 0, 'Unrepresentable power')
    return result


def _ratio(db):
    try:
        result = 10 ** (number(db) / 10)
    except OverflowError as error:
        raise ValueError('Unrepresentable ratio') from error
    require(math.isfinite(result) and result > 0, 'Unrepresentable ratio')
    return result


def expected_parameters(profile):
    """Return full COM paths mapped to expected scalar or table SI values."""
    require(isinstance(profile.get('name'), str) and bool(profile['name']), 'Missing profile name')
    powers, phases = profile['power_dbm'], profile['phase_deg']
    require(isinstance(powers, list) and isinstance(phases, list), 'Tables must be lists')
    require(len(powers) == len(phases) and len(powers) >= 2, 'Invalid table length')
    powers = list(map(number, powers))
    phases = list(map(number, phases))
    require(all(a < b for a, b in zip(powers, powers[1:])), 'Power table must increase')
    frequency = number(profile['frequency_hz'])
    require(frequency > .5, 'Carrier frequency must exceed spectral half-width')
    gain = number(profile['gain_db'])
    count = profile['expected_points']
    require(type(count) is int and count >= 2, 'Invalid expected sweep count')
    start, stop, step = map(number, (profile['sweep_input_start_dbm'],
                                    profile['sweep_input_stop_dbm'], profile['sweep_step_db']))
    require(step > 0 and stop > start, 'Invalid sweep interval')
    require(abs(start + (count - 1) * step - stop) <= 1e-10 * max(1, abs(start), abs(stop)),
            'Sweep count does not span registered limits')
    return {
        AMP + 'AMtoPM_Mode': 2,
        AMP + 'G': _ratio(gain),
        AMP + 'OP1dB': _power(profile['output_p1db_dbm']),
        AMP + 'OPSAT': _power(profile['output_saturation_dbm']),
        AMP + 'OIP2': _power(profile['output_ip2_dbm']),
        AMP + 'OIP3': _power(profile['output_ip3_dbm']),
        AMP + 'PwrAMtoPMpoints': list(map(_power, powers)),
        AMP + 'AMtoPMpoints': [math.radians(p) for p in phases],
        SOURCE + 'Pwr': _power(number(profile['ordinary_output_target_dbm']) - gain),
        SOURCE + 'Phase': math.radians(number(profile['source_phase_deg'])),
        SOURCE + 'Freq': frequency,
        SWEEP + 'StartPwr': _power(start),
        SWEEP + 'StopPwr': _power(stop),
        SWEEP + 'StepPwr': _ratio(step),
        SWEEP + 'AutoLimits': 0,
        SWEEP + 'Freq': frequency,
        SWEEP + 'SweepType': 3,
    }


def normalize(capture, profile):
    """Validate all registered observations, retaining any single extra final step."""
    expected = expected_parameters(profile)
    require(capture.get('profile') == profile['name'], 'Wrong profile')
    require(capture.get('analysis_target') == ANALYSIS, 'Wrong analysis target')
    require(type(capture.get('analysis_done')) is int and capture['analysis_done'] == 1,
            'Incomplete analysis')
    errors = capture.get('manager_errors')
    require(isinstance(errors, str) and not errors.strip(), 'Diagnostic run')
    start = archive.parse_utc(capture['run_started_utc'])
    end = archive.parse_utc(capture['run_returned_utc'])
    require(start <= end, 'Reversed run times')
    nodes = {}
    require(isinstance(capture.get('nodes'), list), 'Missing nodes')
    for item in capture['nodes']:
        require(isinstance(item, dict) and isinstance(item.get('path'), str), 'Invalid node')
        nodes.setdefault(item['path'], []).append(item)

    def node(path):
        require(len(nodes.get(path, [])) == 1, 'Missing or duplicate node: ' + path)
        return nodes[path][0]

    def vector(path, length=None):
        item = node(path)
        value = item.get('data')
        require(isinstance(value, list) and item.get('dimensions') == [len(value)],
                'Invalid vector shape: ' + path)
        require(length is None or len(value) == length, 'Invalid vector length: ' + path)
        return value

    def numeric_vector(path, length=None):
        return list(map(number, vector(path, length)))

    node(ANALYSIS)
    timestamps = {}
    for path in (PATH + '1', PATH + '2', RAW):
        stamp = node(path).get('timestamp')
        require(type(stamp) is int or isinstance(stamp, str) and re.fullmatch(r'[0-9]+', stamp),
                'Invalid dataset timestamp: ' + path)
        stamp = int(stamp)
        require(math.floor(start) <= stamp <= math.ceil(end), 'Stale dataset: ' + path)
        timestamps[path] = stamp
    readbacks = {}
    for path, wanted in expected.items():
        item = node(path)
        actual = item.get('data')
        if isinstance(wanted, list):
            require(isinstance(actual, list) and len(actual) == len(wanted)
                    and item.get('dimensions') == [1, len(wanted)],
                    'Invalid table array shape: ' + path)
            actual = list(map(number, actual))
            pairs = zip(actual, wanted)
        else:
            require(item.get('dimensions') is None, 'Invalid scalar shape: ' + path)
            actual = number(actual)
            pairs = [(actual, wanted)]
        require(all(abs(a - b) <= 1e-12 * max(1e-12, abs(b)) for a, b in pairs),
                'Parameter unit/value mismatch: ' + path)
        readbacks[path] = actual

    frequency = number(profile['frequency_hz'])

    def points(path, count):
        key = path + '/Eqns/VarBlock/'
        require(vector(key + 'NodeNames', 2 * count) == ['1', '2'] * count, 'Wrong nodes')
        require(numeric_vector(key + 'CF', 2 * count) == [frequency] * (2 * count),
                'Wrong channel frequency')
        power = numeric_vector(key + 'DCP', 2 * count)
        phase = numeric_vector(key + 'DCPH', 2 * count)
        require(all(p > 0 for p in power), 'Nonpositive channel power')
        return [dict(index=i, input_power_w=power[2 * i], output_power_w=power[2 * i + 1],
                     input_phase_rad=phase[2 * i], output_phase_rad=phase[2 * i + 1])
                for i in range(count)]

    ordinary = points(PATH + '1', 1)[0]
    require(abs(ordinary['input_phase_rad'] - expected[SOURCE + 'Phase']) < 1e-10,
            'Ordinary phase mismatch')
    ordinary_input_dbm = 10 * math.log10(ordinary['input_power_w']) + 30
    require(abs(ordinary_input_dbm - (profile['ordinary_output_target_dbm'] - profile['gain_db']))
            < 1e-5, 'Ordinary input power mismatch')
    expected_count = profile['expected_points']
    size = len(vector(PATH + '2/Eqns/VarBlock/DCP'))
    require(size in (2 * expected_count, 2 * (expected_count + 1)), 'Unexpected sweep size')
    rows = points(PATH + '2', size // 2)
    start_decimal = Decimal(str(profile['sweep_input_start_dbm']))
    step_decimal = Decimal(str(profile['sweep_step_db']))
    gain_decimal = Decimal(str(profile['gain_db']))
    for index, row in enumerate(rows):
        nominal_decimal = start_decimal + step_decimal * index
        nominal = float(nominal_decimal)
        require(abs(10 * math.log10(row['input_power_w']) + 30 - nominal) < 1e-5,
                'Sweep grid mismatch')
        require(abs(row['input_phase_rad']) < 1e-10, 'Unexpected sweep source phase')
        row['nominal_output_dbm'] = float(nominal_decimal + gain_decimal)

    key = RAW + '/Eqns/VarBlock/'
    names = vector(key + 'IDName')
    require(all(isinstance(name, str) and name for name in names), 'Invalid signal names')
    ids = numeric_vector(key + 'IDNo', len(names))
    require(all(v.is_integer() for v in ids) and len(set(ids)) == len(ids), 'Invalid signal IDs')
    mapping = dict(zip(ids, names))
    desired = [identifier for identifier, name in mapping.items()
               if name.startswith('{1}D[') and name.endswith(',Source,RFAmp')]
    require(len(desired) == 1, 'Ambiguous desired signal')
    frequencies = numeric_vector(key + 'F2')
    count = len(frequencies)
    require(count > 0 and all(f >= 0 for f in frequencies), 'Invalid raw frequencies')
    labels = numeric_vector(key + 'ID2', count)
    require(all(label in mapping for label in labels), 'Unknown raw signal ID')
    powers = numeric_vector(key + 'P2', count)
    require(all(p >= 0 for p in powers), 'Negative raw power')
    volts = numeric_vector(key + 'V2', 2 * count)
    impedances = numeric_vector(key + 'Z2', 2 * count)
    carrier = []
    for index, label in enumerate(labels):
        if label != desired[0]:
            continue
        voltage = complex(*volts[2 * index:2 * index + 2])
        impedance = complex(*impedances[2 * index:2 * index + 2])
        power = powers[index]
        require(impedance != 0 and power > 0, 'Invalid desired carrier power/impedance')
        physical_power = .5 * (voltage.real * voltage.real + voltage.imag * voltage.imag) * (1 / impedance).real
        require(math.isfinite(physical_power) and physical_power > 0
                and archive.relative(physical_power, power) <= 1e-12, 'Raw V/Z/P inconsistency')
        phase = math.degrees(math.atan2(voltage.imag, voltage.real))
        require(archive.hypotheses.circular_error(phase, math.degrees(ordinary['output_phase_rad']))
                <= 1e-8, 'Raw versus ordinary phase mismatch')
        carrier.append(dict(frequency_hz=frequencies[index], power_w=power,
                            voltage=[voltage.real, voltage.imag], impedance=[impedance.real, impedance.imag],
                            output_phase_deg=phase, label=mapping[label]))
    require(len(carrier) == 2 and sorted(c['frequency_hz'] for c in carrier)
            == [frequency - .5, frequency + .5], 'Unexpected raw carrier support')
    require(archive.relative(carrier[0]['power_w'], carrier[1]['power_w']) <= 1e-12,
            'Nonconstant raw spectrum')
    ordinary['raw_carrier'] = carrier
    ordinary['raw_output_w'] = carrier[0]['power_w'] / 2 + carrier[1]['power_w'] / 2
    ordinary['raw_vs_report_relative_error'] = archive.relative(ordinary['raw_output_w'], ordinary['output_power_w'])
    return dict(profile=copy.deepcopy(profile), analysis_target=ANALYSIS,
                run_started_utc=capture['run_started_utc'], run_returned_utc=capture['run_returned_utc'],
                timestamps=timestamps, parameters_si=readbacks, ordinary=ordinary,
                sweep=rows[:expected_count], additional_sweep_points=rows[expected_count:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('plan', type=Path)
    parser.add_argument('profile')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    require(not args.output.exists(), 'Output already exists')
    plan = archive.load(args.plan)
    profiles = [profile for profile in plan['profiles'] if profile['name'] == args.profile]
    require(len(profiles) == 1, 'Missing or duplicate planned profile')
    result = normalize(archive.load(args.capture), profiles[0])
    with args.output.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print('Validated', args.profile, 'with', len(result['sweep']), 'registered points and',
          len(result['additional_sweep_points']), 'extra endpoint(s)')


if __name__ == '__main__':
    main()

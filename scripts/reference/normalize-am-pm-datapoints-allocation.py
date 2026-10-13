"""Retain and validate ordinary input carriers for the allocation study; no COM."""

import argparse
import copy
import importlib.util
import json
import math
from pathlib import Path
import re


spec = importlib.util.spec_from_file_location(
    'allocation_previous_normalizer', Path(__file__).with_name('archive-am-pm-datapoints-zero-study.py'))
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)
require, number = previous.require, previous.number
expected_parameters = previous.expected_parameters
BASE, ANALYSIS, AMP, SOURCE = previous.BASE, previous.ANALYSIS, previous.AMP, previous.SOURCE
SWEEP, PATH, RAW = previous.SWEEP, previous.PATH, previous.RAW
INPUT_VARIABLES = ('F1', 'P1', 'V1', 'Z1', 'ID1', 'IDNo', 'IDName')
INPUT_PREFIX = RAW + '/Eqns/VarBlock/'
INPUT_PATHS = frozenset(INPUT_PREFIX + name for name in INPUT_VARIABLES)
INPUT_LABEL = re.compile(r'\{1\}D\[[^\[\]\r\n]+\],Source')
POWER_TOLERANCE = 1e-12
PHASE_TOLERANCE_DEGREES = 1e-8


def _exact(left, right):
    """Raw JSON values retain their types, signed zero and every floating bit."""
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(_exact(left[key], right[key]) for key in left)
    if isinstance(left, list):
        return len(left) == len(right) and all(_exact(a, b) for a, b in zip(left, right))
    if isinstance(left, float):
        return math.isfinite(left) and math.isfinite(right) and left.hex() == right.hex()
    return left == right


def _input_carriers(nodes, frequency, input_phase_rad):
    """Validate seven retained raw nodes and select the exact source-only signal."""
    require(isinstance(nodes, list) and len(nodes) == len(INPUT_PATHS), 'Incomplete raw input-spectrum nodes')
    by_path = {}
    for item in nodes:
        require(isinstance(item, dict) and isinstance(item.get('path'), str)
                and item['path'] in INPUT_PATHS, 'Unexpected raw input-spectrum node')
        path = item['path']
        require(path not in by_path, 'Duplicate raw input-spectrum node: ' + path)
        error = item.get('evaluation_error')
        require(error is None or isinstance(error, str) and not error.strip(), 'Raw input node evaluation error')
        by_path[path] = item
    require(set(by_path) == INPUT_PATHS, 'Incomplete raw input-spectrum paths')

    def vector(name, length=None):
        item = by_path[INPUT_PREFIX + name]
        values, dimensions = item.get('data'), item.get('dimensions')
        require(isinstance(values, list) and isinstance(dimensions, list) and len(dimensions) == 1
                and type(dimensions[0]) is int and dimensions[0] == len(values),
                'Invalid raw input vector shape: ' + name)
        require(length is None or len(values) == length, 'Invalid raw input vector length: ' + name)
        return values

    def numeric_vector(name, length=None):
        values = vector(name, length)
        for value in values:
            number(value)
        return values

    names = vector('IDName')
    require(all(isinstance(name, str) and name for name in names), 'Invalid raw input signal names')
    identifiers = numeric_vector('IDNo', len(names))
    require(all(number(value).is_integer() and value == int(number(value)) for value in identifiers)
            and len(set(identifiers)) == len(identifiers), 'Invalid or duplicate raw input signal IDs')
    mapping = dict(zip(identifiers, names))
    desired = [identifier for identifier, label in mapping.items() if INPUT_LABEL.fullmatch(label)]
    require(len(desired) == 1, 'Missing or ambiguous desired input signal')
    frequencies = numeric_vector('F1')
    count = len(frequencies)
    require(count > 0 and all(value >= 0 for value in frequencies), 'Invalid raw input frequencies')
    labels = numeric_vector('ID1', count)
    require(all(number(value).is_integer() and value == int(number(value)) and value in mapping for value in labels),
            'Unknown or invalid raw input signal ID')
    powers = numeric_vector('P1', count)
    require(all(value >= 0 for value in powers), 'Negative raw input power')
    volts = numeric_vector('V1', 2 * count)
    impedances = numeric_vector('Z1', 2 * count)
    indices = [index for index, label in enumerate(labels) if label == desired[0]]
    require(len(indices) == 2, 'Expected exactly two desired raw input carriers')
    frequency, input_phase_rad = number(frequency), number(input_phase_rad)
    require(frequency > .5 and frequency - .5 < frequency + .5, 'Invalid raw input carrier frequency')
    require(sorted(frequencies[index] for index in indices) == [frequency - .5, frequency + .5],
            'Unexpected raw input carrier support')
    carriers = []
    for index in indices:
        voltage_values, impedance_values = volts[2 * index:2 * index + 2], impedances[2 * index:2 * index + 2]
        voltage = complex(*map(number, voltage_values))
        impedance = complex(*map(number, impedance_values))
        power = powers[index]
        require(impedance != 0 and power > 0, 'Invalid desired raw input power/impedance')
        try:
            physical = .5 * (voltage.real * voltage.real + voltage.imag * voltage.imag) * (1 / impedance).real
        except (OverflowError, ZeroDivisionError) as error:
            raise ValueError('Unrepresentable raw input V/Z/P') from error
        require(math.isfinite(physical) and physical > 0
                and previous.archive.relative(physical, power) <= POWER_TOLERANCE,
                'Raw input V/Z/P inconsistency')
        phase = math.degrees(math.atan2(voltage.imag, voltage.real))
        require(previous.archive.hypotheses.circular_error(phase, math.degrees(input_phase_rad))
                <= PHASE_TOLERANCE_DEGREES, 'Raw versus ordinary input phase mismatch')
        carriers.append(dict(raw_index=index, signal_id=labels[index], frequency_hz=frequencies[index],
                             power_w=power, voltage=copy.deepcopy(voltage_values),
                             impedance=copy.deepcopy(impedance_values), label=mapping[labels[index]]))
    require(previous.archive.relative(carriers[0]['power_w'], carriers[1]['power_w']) <= POWER_TOLERANCE,
            'Nonconstant raw input spectrum')
    raw_power = carriers[0]['power_w'] / 2 + carriers[1]['power_w'] / 2
    require(math.isfinite(raw_power) and raw_power > 0, 'Invalid mean raw input power')
    return carriers, raw_power


def validate_raw_input(sample):
    """Validate the new raw fields independently; do not mutate or rescore them.

    The ordinary DCP input and source readback remain distinct observations.
    No equality gate between DCP and raw input power is imposed here. Callers
    may report their difference; gain, logarithms and phase diagnostics belong
    in the scientific score, not these retained raw values.
    """
    require(isinstance(sample, dict) and isinstance(sample.get('profile'), dict)
            and isinstance(sample.get('ordinary'), dict), 'Missing normalized input context')
    profile, ordinary = sample['profile'], sample['ordinary']
    require('frequency_hz' in profile and 'input_phase_rad' in ordinary
            and all(name in ordinary for name in ('raw_input_spectrum_nodes', 'raw_input_carrier', 'raw_input_w')),
            'Missing normalized raw input fields')
    carriers, raw_power = _input_carriers(ordinary['raw_input_spectrum_nodes'], profile['frequency_hz'],
                                        ordinary['input_phase_rad'])
    require(_exact(ordinary['raw_input_carrier'], carriers), 'Retained raw input carriers differ from original nodes')
    require(_exact(ordinary['raw_input_w'], raw_power), 'Retained raw input mean power changed')


def normalize(capture, profile):
    """Apply all frozen output/acquisition guards, then append raw input evidence."""
    sample = previous.normalize(capture, profile)
    nodes = [copy.deepcopy(node) for node in capture['nodes'] if node['path'] in INPUT_PATHS]
    carriers, raw_power = _input_carriers(nodes, profile['frequency_hz'], sample['ordinary']['input_phase_rad'])
    sample['ordinary'].update(raw_input_spectrum_nodes=nodes, raw_input_carrier=carriers, raw_input_w=raw_power)
    validate_raw_input(sample)
    return sample


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('plan', type=Path)
    parser.add_argument('profile')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    require(not args.output.exists(), 'Output already exists')
    plan = previous.archive.load(args.plan)
    profiles = [profile for profile in plan['profiles'] if profile['name'] == args.profile]
    require(len(profiles) == 1, 'Missing or duplicate planned profile')
    result = normalize(previous.archive.load(args.capture), profiles[0])
    payload = json.dumps(result, indent=2, allow_nan=False) + '\n'
    with args.output.open('x', encoding='utf-8', newline='\n') as stream:
        stream.write(payload)
    print('Validated', args.profile, 'with', len(result['ordinary']['raw_input_carrier']),
          'raw input carriers and', len(result['sweep']), 'registered sweep points')


if __name__ == '__main__':
    main()

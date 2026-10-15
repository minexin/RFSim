"""Independent direct-array and multinomial audit of the pre-C frozen artifact.

This intentionally imports neither the pilot scorer nor the native transport.
Optional original raw directories additionally prove the selected archive bytes.
"""
import argparse
import cmath
import hashlib
import itertools
import json
import math
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def node(capture, suffix):
    rows = [n for n in capture['nodes'] if n['path'].endswith('/' + suffix)]
    require(len(rows) == 1, 'Missing/ambiguous direct-array node: ' + suffix)
    return rows[0]


def array(capture, name):
    return node(capture, 'System3_Design3_Data/Eqns/VarBlock/' + name)['data']


def observed(capture, port, origin, *, fixed_input=False):
    ids, labels = array(capture, 'IDNo'), array(capture, 'IDName')
    names = dict(zip(ids, labels))
    require(len(ids) == len(labels) == len(names), 'Invalid original identity mapping')
    frequencies, powers, voltages, impedances, row_ids = (array(capture, n + str(port)) for n in ('F', 'P', 'V', 'Z', 'ID'))
    result = []
    for i, identifier in enumerate(row_ids):
        label = names[identifier]
        if '}' not in label or label.split('}', 1)[1] != origin:
            continue
        voltage = complex(*voltages[2*i:2*i+2])
        impedance = complex(*impedances[2*i:2*i+2])
        require(impedance.real > 0 and abs(impedance.imag) <= 1e-7, 'Unexpected impedance')
        physical_power = .5 * abs(voltage)**2 * (1 / impedance).real
        require(abs(physical_power / powers[i] - 1) <= 1e-10, 'Raw voltage/power disagreement')
        wave = voltage / (10. if fixed_input else cmath.sqrt(2 * impedance))
        result.append((frequencies[i], wave))
    require(len(result) == 2, 'Expected two original band-edge rows')
    return sorted(result)


def origin(counts):
    parts = []
    # Same physical contributor ordering as the raw labels, built independently
    # from signed count tuples, not from the production origin formatter.
    for index, count in zip((-11, -10, 10, 11), counts):
        if count:
            term = (str(count) + 'x' if count > 1 else '') + '(Source.Source' + str(abs(index) - 9) + ')'
            parts.append(('-' if index < 0 else '+' if parts else '') + term)
    return '[' + ''.join(parts) + '],RFAmp1'


def audit(ab_path, freeze_path, raw_runs=None):
    ab, frozen = read(ab_path), read(freeze_path)
    require(sha(ab_path) == frozen['ab_archive_sha256'], 'A/B archive hash differs from freeze')
    require(frozen['relative_complex_wave_tolerance'] == 1e-7, 'Scientific tolerance changed')
    raw_verified = []
    identification = {}
    for packet in ab['runs']:
        capture = packet['capture']
        if raw_runs is not None:
            raw_file = Path(raw_runs) / packet['run_id'] / 'capture.json'
            raw = read(raw_file)
            require(sha(raw_file) == capture['raw_capture_sha256'], 'Original capture bytes changed')
            indexed = {n['path']: n for n in raw['nodes']}
            require(all(indexed[n['path']] == n for n in capture['nodes']), 'Selected archive altered an original node')
            raw_verified.append(packet['run_id'])
        input_rows = observed(capture, 1, 'D[(Source.Source1)],Source', fixed_input=True)
        require([r[0] for r in input_rows] == [1e9 - .5, 1e9 + .5], 'Incorrect input support')
        wave = sum(r[1] for r in input_rows) / 2
        gain = node(capture, 'RFAmp1/ParamSet/G')['data']
        op1 = node(capture, 'RFAmp1/ParamSet/OP1dB')['data']
        require(abs(wave)**2 < op1 / gain * 10**(-.4), 'Identification no longer below limiter knee')
        fit = frozen['A' if packet['run_id'] == 'A-baseline' else 'B']
        errors = []
        for n in (5, 7, 9, 11):
            rows = observed(capture, 3, '[' + str(n) + 'x(Source.Source1)],RFAmp1')
            require([r[0] for r in rows] == [n*1e9 - n/2, n*1e9 + n/2], 'Incorrect harmonic support')
            coefficient = sum(r[1] for r in rows) / (2 * 5**(n-1) * wave**n)
            expected = fit['coefficients_c0_to_c11'][n]
            errors.append(abs(coefficient / expected - 1))
        require(max(errors) <= 1e-12, 'Independent coefficient extraction disagrees')
        identification[packet['run_id']] = {'input_wave': [wave.real, wave.imag],
                                            'max_relative_coefficient_error': max(errors)}

    source_rows = frozen['C_prospective_input_assumption']
    waves = [complex(*r['wave']) for r in source_rows]
    drive = sum(abs(w)**2 for w in waves)
    knee = math.sqrt(10**((21 - 10 - 34)/10))
    headroom = math.sqrt(10**((23 - 10 - 31)/10)) - knee
    radius = math.sqrt(drive)
    scale = 1 if radius <= knee else (knee + headroom * math.tanh((radius - knee) / headroom)) / radius
    require(abs(scale / frozen['C_prospective_prediction']['operating_point']['nonlinear_input_scale'] - 1) <= 1e-12,
            'Independent common limiter disagrees')
    signed = [waves[1].conjugate() * scale, waves[0].conjugate() * scale,
              waves[0] * scale, waves[1] * scale]
    predicted = {t['label']: t for t in frozen['C_prospective_prediction']['terms']}
    checked, errors, counts_by_order = set(), [], {}
    for n in (5, 7, 9, 11):
        counts_by_order[str(n)] = 0
        for a, b, c in itertools.product(range(n+1), repeat=3):
            d = n-a-b-c
            if d < 0:
                continue
            counts = (a, b, c, d)
            frequency_bin = -11*a - 10*b + 10*c + 11*d
            if frequency_bin <= 0:
                continue
            label = origin(counts)
            require(label in predicted and label not in checked, 'Independent origin mismatch')
            checked.add(label)
            term = predicted[label]
            require(term['order'] == n and term['bin'] == frequency_bin and term['bandwidth_hz'] == n,
                    'Independent term support/order mismatch')
            multiplicity = math.factorial(n) // math.prod(math.factorial(k) for k in counts)
            wave = frozen['B']['coefficients_c0_to_c11'][n] * 5**(n-1) * multiplicity
            for value, count in zip(signed, counts):
                wave *= value**count
            errors.append(abs(wave / complex(*term['expected_wave']) - 1))
            counts_by_order[str(n)] += 1
    require(checked == set(predicted) and len(checked) == 380, 'Incomplete independent C prediction audit')
    require(max(errors) <= 1e-12, 'Independent multinomial prediction disagrees')
    return {'classification': 'independent_pre_C_direct_array_and_multinomial_audit',
            'ab_sha256': sha(ab_path), 'freeze_sha256': sha(freeze_path),
            'raw_captures_verified': raw_verified, 'identification': identification,
            'C_origin_counts': counts_by_order, 'C_total_origins': len(checked),
            'max_relative_prediction_error': max(errors), 'common_input_scale': scale,
            'scientific_relative_wave_tolerance': 1e-7, 'passed': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ab', required=True, type=Path)
    parser.add_argument('--freeze', required=True, type=Path)
    parser.add_argument('--raw-runs', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = audit(args.ab, args.freeze, args.raw_runs)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

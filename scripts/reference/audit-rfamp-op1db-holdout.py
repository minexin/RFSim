"""Independent C/D audit from original arrays, without the scorer or native code."""
import importlib.util
import itertools
import math
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    'op1db_direct_arrays', Path(__file__).with_name('audit-rfamp-op1db-freeze.py'))
direct = importlib.util.module_from_spec(spec)
spec.loader.exec_module(direct)
require = direct.require


def audit(frozen, packets, raw_runs=None):
    a, b, c, d = packets
    require(frozen['relative_complex_wave_tolerance'] == 1e-7, 'Changed scientific tolerance')
    verified = []
    if raw_runs is not None:
        for packet in packets:
            raw_path = Path(raw_runs) / packet['run_id'] / 'capture.json'
            raw = direct.read(raw_path)
            require(direct.sha(raw_path) == packet['capture']['raw_capture_sha256'],
                    'Original capture hash changed')
            nodes = {n['path']: n for n in raw['nodes']}
            require(all(nodes[n['path']] == n for n in packet['capture']['nodes']),
                    'Selected archive altered an original node')
            for name, expected in packet['files_sha256'].items():
                require(direct.sha(raw_path.parent / name) == expected, 'Original raw file hash changed')
            verified.append(packet['run_id'])

    capture = c['capture']
    waves = []
    for index, frequency in ((1, 1e9), (2, 1.1e9)):
        rows = direct.observed(capture, 1, f'D[(Source.Source{index})],Source', fixed_input=True)
        require([r[0] for r in rows] == [frequency - .5, frequency + .5], 'Input support mismatch')
        require(abs(rows[0][1] / rows[1][1] - 1) <= 1e-7, 'Input band edges disagree')
        waves.append(sum(r[1] for r in rows) / 2)
    # Independent transcription of the already frozen common-response formula.
    # Coefficients are read verbatim; no C output enters this computation.
    radius = math.sqrt(sum(abs(w)**2 for w in waves))
    knee = math.sqrt(10**((21 - 10 - 34)/10))
    headroom = math.sqrt(10**((23 - 10 - 31)/10)) - knee
    scale = 1 if radius <= knee else (knee + headroom * math.tanh((radius - knee) / headroom)) / radius
    signed = [waves[1].conjugate() * scale, waves[0].conjugate() * scale,
              waves[0] * scale, waves[1] * scale]
    checked, errors, by_order = set(), [], {}
    for n in (5, 7, 9, 11):
        by_order[str(n)] = {'origins': 0, 'passed': 0, 'max_relative_wave_error': 0.}
        for x, y, z in itertools.product(range(n + 1), repeat=3):
            w = n - x - y - z
            if w < 0:
                continue
            counts = (x, y, z, w)
            frequency_bin = -11*x - 10*y + 10*z + 11*w
            if frequency_bin <= 0:
                continue
            label = direct.origin(counts)
            require(label not in checked, 'Duplicate independent origin')
            checked.add(label)
            rows = direct.observed(capture, 3, label)
            require([r[0] for r in rows] == [frequency_bin*1e8 - n/2, frequency_bin*1e8 + n/2],
                    'Holdout support mismatch')
            multiplicity = math.factorial(n) // math.prod(math.factorial(k) for k in counts)
            expected = frozen['B']['coefficients_c0_to_c11'][n] * 5**(n-1) * multiplicity
            for value, count in zip(signed, counts):
                expected *= value**count
            error = max(abs(row[1] / expected - 1) for row in rows)
            errors.append(error)
            summary = by_order[str(n)]
            summary['origins'] += 1
            summary['passed'] += error <= 1e-7
            summary['max_relative_wave_error'] = max(summary['max_relative_wave_error'], error)
    require(len(checked) == 380, 'Incomplete independent holdout audit')
    restored, identities, raw_identifiers = {}, {}, {}
    for port in (1, 3, 2):
        fields = [name + str(port) for name in ('F', 'P', 'V', 'Z')]
        restored[str(port)] = all(direct.array(a['capture'], key) == direct.array(d['capture'], key)
                                 for key in fields)
        label_rows = []
        for packet in (a, d):
            data = packet['capture']
            numbers, names = direct.array(data, 'IDNo'), direct.array(data, 'IDName')
            mapping = dict(zip(numbers, names))
            require(len(numbers) == len(names) == len(mapping), 'Invalid raw identity table')
            label_rows.append([mapping[k] for k in direct.array(data, 'ID' + str(port))])
        identities[str(port)] = label_rows[0] == label_rows[1]
        raw_identifiers[str(port)] = (direct.array(a['capture'], 'ID' + str(port))
                                     == direct.array(d['capture'], 'ID' + str(port)))
    return {'classification': 'independent_C_D_direct_array_multinomial_audit',
            'raw_runs_verified': verified, 'raw_files_verified': 6 * len(verified),
            'C_origins': len(checked), 'C_band_edge_rows': 2 * len(checked),
            'C_by_order': by_order, 'C_max_relative_wave_error': max(errors),
            'C_common_input_scale': scale, 'C_coefficient_refit': False,
            'D_original_numeric_arrays_exact_A': restored, 'D_origin_names_exact_A': identities,
            'D_raw_numeric_id_arrays_exact_A': raw_identifiers,
            'D_identity_note': 'Raw numeric lookup IDs are run-local; compare their original mapped names, including coherence groups.',
            'D_independent_predictions': 0,
            'scientific_relative_complex_wave_tolerance': 1e-7,
            'passed': max(errors) <= 1e-7 and all(restored.values()) and all(identities.values())}

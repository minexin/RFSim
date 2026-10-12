"""Independent mixer/Fourier, resistor V/I and thermal C/P example verification.

This imports no RFModel code. Read one saved JSON result, or run the C++ example
at three reference scalings and verify deterministic replay and failure inputs.
"""
import argparse
import cmath
import copy
import hashlib
import json
import math
from pathlib import Path
import subprocess


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify(result):
    require(result['format'] == 'rfmodel.mixed-reference-conversion-example', 'Unexpected format')
    parameters = result['parameters']
    require(parameters == {'spacing_hz': 1e8, 'reference_ohms': 50, 'resistance_ohms': 25,
                           'temperature_k': 290, 'rf_bin': 8, 'lo_bin': 10,
                           'phase_radians': .4, 'source_noise_w_per_hz': 1e-20,
                           'source': [.01, .005]}, 'Example physical inputs changed')
    require(result['output_bins'] == [2, 18], 'Missing folded or upper sideband')
    scale = result['reference_scale']
    require(math.isfinite(scale) and scale > 0, 'Invalid reference scale')
    wave_errors, noise_errors = [], []

    def close(actual, expected, noise=False):
        error = abs(actual - expected)
        require(math.isfinite(error) and error <= (1e-30 if noise else 1e-13) + 1e-10 * abs(expected),
                'Independent noise mismatch' if noise else 'Independent physical wave mismatch')
        (noise_errors if noise else wave_errors).append(error)

    def vector(name, length):
        value = result[name]
        require(len(value) == length, f'{name} size differs')
        require(all(isinstance(x, list) and len(x) == 2 for x in value), f'{name} requires complex pairs')
        return [complex(*x) for x in value]

    refs = vector('reference_impedances_ohms', 7)
    nominal = [31+12j, 73-21j, 19+8j, 110+31j, 43-12j, 22+7j, 91-18j]
    for actual, expected in zip(refs, nominal):
        close(actual, scale * expected)
    local_a = vector('incident_local', 7)
    local_b = vector('outgoing_local', 7)
    voltage = vector('voltage_rms', 7)
    current = vector('current_rms', 7)
    for index, reference in enumerate(refs):
        close(voltage[index], (reference.conjugate()*local_a[index] + reference*local_b[index]) / math.sqrt(reference.real))
        close(current[index], (local_a[index]-local_b[index])/math.sqrt(reference.real))
        close((voltage[index]*current[index].conjugate()).real,
              abs(local_a[index])**2 - abs(local_b[index])**2)
    for left, right in [(1, 3), (2, 5)]:
        close(voltage[left], voltage[right])
        close(current[left], -current[right])
    for left, right in [(3, 4), (5, 6)]:
        close(voltage[left] - voltage[right], 25 * current[left])
        close(current[left], -current[right])
    source = complex(*parameters['source'])
    pump = cmath.exp(1j * parameters['phase_radians'])
    generated = [pump * source.conjugate(), pump * source]
    transmission = 2 / (2 + 25 / 50)
    expected = [transmission * x for x in generated]
    close(voltage[0] + 50*current[0], 2*math.sqrt(50)*source)
    close(voltage[0] - 50*current[0], 0)
    for index, value in zip([1, 2], generated):
        close((voltage[index]-50*current[index])/(2*math.sqrt(50)), value)
    common_a = vector('incident_common', 2)
    common_b = vector('outgoing_common', 2)
    for k, index in enumerate([4, 6]):
        close(common_a[k], 0)
        close(common_b[k], expected[k])
        close((voltage[index]-50*current[index])/(2*math.sqrt(50)), expected[k])
        close(-(voltage[index]*current[index].conjugate()).real, abs(expected[k])**2)
    kt = 1.380649e-23 * 290
    reflection = 25 / (2*50 + 25)
    thermal = kt * (1 - transmission**2 - reflection**2)
    total = transmission**2 * parameters['source_noise_w_per_hz'] + thermal
    pair = transmission**2 * pump**2 * parameters['source_noise_w_per_hz']
    for name, reference in [('covariance_common_w_per_hz', [[total, 0], [0, total]]),
                            ('complementary_common_w_per_hz', [[0, pair], [pair, 0]])]:
        matrix = result[name]
        require(len(matrix) == 2 and all(len(row) == 2 for row in matrix), 'Noise matrix size differs')
        for i in range(2):
            for j in range(2):
                close(complex(*matrix[i][j]), reference[i][j], True)
    net = result['net_noise_into_device_w_per_hz']
    require(len(net) == 7, 'Net-noise size differs')
    close(net[0], parameters['source_noise_w_per_hz'], True)
    for left, right in [(1, 3), (2, 5)]:
        close(net[left]+net[right], 0., True)
    for index in [4, 6]:
        close(-net[index], total, True)
    require(math.isfinite(result['relative_residual']) and 0 <= result['relative_residual'] < 1e-12,
            'Solver residual invalid')
    return {'passed': True, 'reference_scale': scale, 'wave_checks': len(wave_errors),
            'noise_checks': len(noise_errors), 'maximum_wave_absolute_error': max(wave_errors),
            'maximum_noise_absolute_error_w_per_hz': max(noise_errors),
            'expected_outgoing': [[x.real, x.imag] for x in expected],
            'expected_output_noise_w_per_hz': total,
            'expected_sideband_complementary_w_per_hz': [pair.real, pair.imag]}


def verify_executable(executable):
    cases = []
    for scale in ['0.5', '1', '2']:
        process = subprocess.run([str(executable), scale], check=True, capture_output=True)
        document = json.loads(process.stdout)
        report = verify(document)
        report['output_sha256'] = hashlib.sha256(process.stdout).hexdigest()
        cases.append(report)
        replay = subprocess.run([str(executable), scale], check=True, capture_output=True)
        require(replay.stdout == process.stdout, 'Repeated example bytes differ')
    rejected = []
    for mutation in ['wave', 'complementary_noise', 'voltage', 'net_noise']:
        changed = copy.deepcopy(document)
        if mutation == 'wave':
            changed['outgoing_common'][0][0] += .001
        elif mutation == 'complementary_noise':
            changed['complementary_common_w_per_hz'][0][1][1] += 1e-21
        elif mutation == 'voltage':
            changed['voltage_rms'][3][0] += .001
        else:
            changed['net_noise_into_device_w_per_hz'][4] += 1e-21
        try:
            verify(changed)
        except ValueError:
            rejected.append(mutation)
        else:
            raise ValueError('Verifier accepted corrupted ' + mutation)
    invalid = ['0', '-1', 'nan', '1x']
    for value in invalid:
        process = subprocess.run([str(executable), value], capture_output=True)
        require(process.returncode != 0 and not process.stdout, 'Example accepted invalid scale')
    return {'passed': True, 'method': 'independent Fourier/matched-resistor/thermal C and P formulas plus V/I reconstruction',
            'cases': cases, 'byte_replay': True, 'verifier_rejects_corruption': rejected,
            'example_rejects_invalid_scalings': invalid,
            'executable_sha256': hashlib.sha256(executable.read_bytes()).hexdigest()}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('result', type=Path, nargs='?')
    parser.add_argument('--executable', type=Path)
    args = parser.parse_args()
    require((args.result is None) != (args.executable is None), 'Select one result or executable')
    report = verify_executable(args.executable.resolve()) if args.executable else verify(
        json.loads(args.result.read_text(encoding='utf-8-sig')))
    print(json.dumps(report, indent=2))

"""Independent quadratic/Fourier and physical RC check for the shipped stage example.

No RFModel imports or native library are used. This checks this example's topology,
not arbitrary coherent graphs. Both files are read-only.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def complex_value(value):
    return complex(*value) if isinstance(value, list) else complex(value)


def verify(document, result):
    require(document['format'] == 'rfmodel.coherent-system' and document['version'] == 1,
            'Expected coherent-system example')
    require(len(document['inputs']) == 1 and len(document['inputs'][0]['components']) == 1,
            'Expected one input carrier')
    require(len(document['stages']) == 2, 'Expected polynomial and RC stages')
    poly, stage = document['stages']
    require(poly['type'] == 'polynomial_amplifier' and stage['type'] == 'linear_network',
            'Unexpected stage types')
    coefficients = poly['voltage_coefficients']
    require(len(coefficients) == 3 and coefficients[0] == 0, 'Expected zero-offset quadratic')
    network = stage['network']
    require(len(network['devices']) == 2 and not network.get('terminations'), 'Expected unloaded RC two-port')
    resistor, capacitor = network['devices']
    require(resistor['model']['type'] == 'resistor' and resistor['model']['connection'] == 'series',
            'Expected series resistor')
    require(capacitor['model']['type'] == 'capacitor' and capacitor['model']['connection'] == 'shunt',
            'Expected shunt capacitor')
    r, c = resistor['id'], capacitor['id']
    require(network['connections'] == [[[r, 1], [c, 0]]] and
            network['external_ports'] == [[r, 0], [c, 1]], 'Unexpected RC wiring')
    require(poly['input'] == document['inputs'][0]['id'] and
            stage['inputs'] == [{'stream': poly['output'], 'port': [r, 0]}], 'Unexpected source path')
    require(len(stage['outputs']) == 1 and stage['outputs'][0]['port'] == [c, 1], 'Unexpected output path')
    output_id = stage['outputs'][0]['id']
    require(document['outputs'] == [output_id], 'Unexpected selected output')
    reference = document.get('reference_ohms', 50.)
    spacing = document['spacing_hz']
    require(result['reference_ohms'] == reference and result['spacing_hz'] == spacing,
            'Result wave reference/grid differs')
    source = document['inputs'][0]['components'][0]
    wave = complex_value(source['amplitude'])
    fundamental = source['bin']
    expected = {fundamental: coefficients[1]*wave,
                2*fundamental: coefficients[2]*math.sqrt(reference/2)*wave*wave}
    # A real waveform has Fourier coefficients sqrt(R/2)*a at positive frequency.
    # Squaring it produces (sqrt(R/2)*a)^2 at twice the frequency.
    streams = {stream['id']: stream for stream in result['streams']}
    before, after = streams[poly['output']], streams[output_id]
    require(before['origins'] == after['origins'], 'Source provenance changed in linear stage')
    identity = lambda stream: sorted((x['bin'], x['kind'], x['bandwidth_hz'], x['coherence_group'])
                                     for x in stream['components'])
    require(identity(before) == identity(after), 'Component identity changed in linear stage')
    resistance = resistor['model']['resistance_ohms']
    capacitance = capacitor['model']['capacitance_f']
    waves, powers = [], []
    reference_rows = []
    for stream, filtered in ((before, False), (after, True)):
        components = stream['components']
        require(len(components) == 2 and {x['bin'] for x in components} == set(expected),
                'Missing or extra deterministic RF term')
        expected_power = {}
        for component in components:
            frequency = component['bin'] * spacing
            value = expected[component['bin']]
            if filtered:
                admittance = 2j * math.pi * frequency * capacitance
                value *= 2 / (2 + resistance/reference + (resistance+reference)*admittance)
            error = abs(complex_value(component['amplitude'])-value)
            require(math.isfinite(error) and error <= 1e-14 + 1e-11*abs(value), 'Wave differs from independent reference')
            waves.append(error)
            expected_power[component['bin']] = abs(value)**2
            if filtered:
                reference_rows.append({'bin':component['bin'],'frequency_hz':frequency,
                                       'amplitude':[value.real,value.imag],'power_w':abs(value)**2})
        require(len(stream['power_by_bin']) == 2 and
                {x['bin'] for x in stream['power_by_bin']} == set(expected_power), 'Power bins differ')
        for row in stream['power_by_bin']:
            value = expected_power[row['bin']]
            error = abs(row['power_w']-value)
            require(math.isfinite(error) and error <= 1e-20 + 1e-11*value, 'Power differs from independent reference')
            powers.append(error)
        value = sum(expected_power.values())
        error = abs(stream['total_power_w']-value)
        require(math.isfinite(error) and error <= 1e-20 + 1e-11*value, 'Total power differs')
        powers.append(error)
    return {'passed':True,'method':'quadratic Fourier coefficients plus physical series-R/shunt-C ABCD',
            'wave_absolute_tolerance':1e-14,'power_absolute_tolerance_w':1e-20,'relative_tolerance':1e-11,
            'wave_values_checked':len(waves),'power_values_checked':len(powers),
            'maximum_wave_absolute_error':max(waves),'maximum_power_absolute_error_w':max(powers),
            'source_origins_preserved':True,'expected_filtered':reference_rows}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('result', type=Path)
    args = parser.parse_args()
    report = verify(json.loads(args.input.read_text(encoding='utf-8-sig')),
                    json.loads(args.result.read_text(encoding='utf-8-sig')))
    report['input_sha256'] = hashlib.sha256(args.input.read_bytes()).hexdigest()
    report['result_sha256'] = hashlib.sha256(args.result.read_bytes()).hexdigest()
    print(json.dumps(report, indent=2))

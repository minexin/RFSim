"""Explicit JSON frequency planning, with seed-indexed data preserved on expansion."""
import copy

from .frequency_plan import integer
from .model_file import _object, _number, _complex, _matrix, _encode
from .operating_point import bilinear_spec, polynomial_spec, amplifier_spec, highorder_spec


def expand_vector(values, mapping, count):
    if not isinstance(values, list) or len(values) != len(mapping):
        raise ValueError('Planned vector must cover exactly the original seed channels')
    output = [0j] * count
    for index, value in zip(mapping, values):
        output[index] = _complex(value)
    return [[z.real, z.imag] for z in output]


def expand_noise(specification, mapping, count):
    from .conversion_network_file import noise_pair
    if specification == {'noiseless': True} and specification.get('noiseless') is True:
        return copy.deepcopy(specification)
    c, p = noise_pair(specification, len(mapping))
    result = []
    for old in (c, p):
        new = [[0j] * count for _ in range(count)]
        for i, row in enumerate(mapping):
            for j, column in enumerate(mapping):
                new[row][column] = old[i][j]
        result.append(_encode(new))
    return dict(covariance=result[0], complementary=result[1])


def reorder_native_channels(native, channels):
    original = native['channels']
    if len(original) != len(channels) or set(original) != set(channels):
        raise ValueError('Actual model ports differ from the frequency-planning declaration')
    lookup = {channel: i for i, channel in enumerate(original)}
    order = [lookup[channel] for channel in channels]
    for name in ('direct', 'conjugate', 'intrinsic_covariance', 'intrinsic_complementary'):
        if name in native:
            matrix = native[name]
            native[name] = [[matrix[i][j] for j in order] for i in order]
    native['channels'] = list(channels)


def prepare_frequency_plan(library, document):
    specification = document['frequency_plan']
    _object(specification, (), ('max_total_channels', 'max_bin', 'max_transitions',
                                'new_channel_boundary'))
    options = {key: specification[key] for key in
               ('max_total_channels', 'max_bin', 'max_transitions') if key in specification}
    default = specification.get('new_channel_boundary')
    if 'new_channel_boundary' in specification:
        _object(default, (), ('reflection', 'noise_w_per_hz', 'noise_temperature_k'))
        reflection = _complex(default.get('reflection', 0))
        if 'noise_w_per_hz' in default and 'noise_temperature_k' in default:
            raise ValueError('Choose one generated boundary noise specification')
        for field in ('noise_w_per_hz', 'noise_temperature_k'):
            if field in default and _number(default[field]) < 0:
                raise ValueError('Generated boundary noise must be nonnegative')
        if 'noise_temperature_k' in default and abs(reflection) > 1:
            raise ValueError('Thermal generated boundaries require passive reflection')
    entries = document['devices']
    descriptions, names, original_channels = [], {}, []
    for entry in entries:
        if (not isinstance(entry, dict) or not isinstance(entry.get('id'), str)
                or not entry['id'] or entry['id'] in names):
            raise ValueError('Frequency device IDs must be unique nonempty strings')
        name = entry['id']
        names[name] = len(names)
        model = entry.get('model')
        if not isinstance(model, dict):
            raise ValueError('Frequency planning requires an explicit model object')
        kind = model.get('type')
        if kind == 'linear':
            _object(entry, ('id', 'bins', 'model', 'noise'),
                    ('frequency_ports', 'source_noise', 'output_offset'))
            _object(model, ('type',), ('device', 's'))
            if ('s' in model) == ('device' in model):
                raise ValueError('Linear planning requires one parameter model or static S matrix')
            if 's' in model:
                port_count = len(_matrix(model['s']))
                if 'frequency_ports' in entry and integer(entry['frequency_ports'], 1, 512) != port_count:
                    raise ValueError('frequency_ports differs from the static matrix')
            else:
                if 'frequency_ports' not in entry:
                    raise ValueError('Parameterized linear planning requires frequency_ports')
                port_count = integer(entry['frequency_ports'], 1, 512)
            ports = list(range(port_count))
            bins = entry['bins']
            if not isinstance(bins, list) or len(bins) > 512:
                raise ValueError('Expected a linear frequency seed array')
            bins = [integer(b, 0, 2147483647) for b in bins]
            if any(a >= b for a, b in zip(bins, bins[1:])):
                raise ValueError('Linear seed bins must strictly increase')
            seeds = [(p, b) for b in bins for p in ports]
            rules = []
            for p in ports[1:]:
                rules.extend([dict(type='copy', input_port=0, output_port=p),
                              dict(type='copy', input_port=p, output_port=0)])
        else:
            _object(entry, ('id', 'channels', 'model', 'noise'),
                    ('source_noise', 'output_offset', 'am_pm'))
            if not isinstance(entry['channels'], list) or len(entry['channels']) > 512:
                raise ValueError('Expected frequency seed channel array')
            seeds = []
            for channel in entry['channels']:
                _object(channel, ('port', 'bin'))
                seeds.append((integer(channel['port'], 0, 1023), integer(channel['bin'], 0, 2147483647)))
            if kind == 'bilinear_real_mixer':
                model_spec = bilinear_spec(model)
                ports = [model_spec[key] for key in ('rf_port', 'lo_port', 'if_port')]
                rules = [dict(type='mixer', rf_port=ports[0], lo_port=ports[1], if_port=ports[2])]
            elif kind == 'polynomial_amplifier':
                model_spec = polynomial_spec(model)
                ports = [model_spec['input_port'], model_spec['output_port']]
                rules = [dict(type='polynomial', input_port=ports[0], output_port=ports[1],
                              voltage_coefficients=model_spec['voltage_coefficients'])]
            elif kind == 'highorder_amplifier':
                model_spec = highorder_spec(model)
                ports = [model_spec.get('input_port', 0), model_spec.get('output_port', 1)]
                rules = [dict(type='highorder', input_port=ports[0], output_port=ports[1],
                              voltage_coefficients=[0, 0] + model_spec['nonlinear_voltage_coefficients'],
                              generating_bins=model_spec['generating_bins'])]
            elif kind == 'saturating_amplifier':
                model_spec = amplifier_spec(model)
                ports = [model_spec['input_port'], model_spec['output_port']]
                rules = [dict(type='copy', input_port=ports[0], output_port=ports[1])]
            else:
                raise ValueError('Model has no automatic frequency rule: ' + str(kind))
        descriptions.append(dict(ports=ports, seeds=seeds, rules=rules))
        original_channels.append(seeds)
    connections, connected = [], set()
    wires = document.get('connections', [])
    if not isinstance(wires, list):
        raise ValueError('Connections must be an array')
    for wire in wires:
        if not isinstance(wire, list) or len(wire) != 2:
            raise ValueError('Connections require two physical port endpoints')
        endpoints = []
        for endpoint in wire:
            if (not isinstance(endpoint, list) or len(endpoint) != 2
                    or not isinstance(endpoint[0], str) or endpoint[0] not in names):
                raise ValueError('Invalid planned connection endpoint')
            port = integer(endpoint[1], 0, 1023)
            endpoints.append((names[endpoint[0]], port))
            connected.add((endpoint[0], port))
        connections.append(tuple(endpoints))
    plan = library.plan_conversion_frequencies(document['spacing_hz'], descriptions, connections, **options)
    expanded = copy.deepcopy(document)
    del expanded['frequency_plan']
    global_mapping, original_keys, all_keys = [], set(), []
    offset = 0
    for d, entry in enumerate(expanded['devices']):
        mapping, channels = plan.seed_channel_indices[d], plan.channels[d]
        global_mapping.extend(offset + index for index in mapping)
        offset += len(channels)
        name = entry['id']
        original_keys.update((name, p, b) for p, b in original_channels[d])
        all_keys.extend((name, p, b) for p, b in channels)
        if entry['model']['type'] == 'linear':
            entry.pop('frequency_ports', None)
            entry['bins'] = sorted({b for p, b in channels})
        else:
            entry['channels'] = [dict(port=p, bin=b) for p, b in channels]
            entry['noise'] = expand_noise(entry['noise'], mapping, len(channels))
        if 'source_noise' in entry:
            entry['source_noise'] = expand_noise(entry['source_noise'], mapping, len(channels))
        if 'output_offset' in entry:
            entry['output_offset'] = expand_vector(entry['output_offset'], mapping, len(channels))
        if 'am_pm' in entry:
            phase = entry['am_pm']
            if not isinstance(phase, dict) or not isinstance(phase.get('output_channels'), list):
                raise ValueError('AM-PM requires an output channel index array')
            phase['output_channels'] = [mapping[integer(i, 0, len(mapping)-1)]
                                        for i in phase['output_channels']]
    if 'additional_source_noise' in expanded:
        expanded['additional_source_noise'] = expand_noise(
            expanded['additional_source_noise'], global_mapping, plan.total_channels)
    operating = expanded.get('operating_point')
    if isinstance(operating, dict) and 'initial_incident' in operating:
        operating['initial_incident'] = expand_vector(
            operating['initial_incident'], global_mapping, plan.total_channels)
    if not isinstance(expanded['boundaries'], list):
        raise ValueError('Boundaries must be an array')
    assigned = set()
    for boundary in expanded['boundaries']:
        if not isinstance(boundary, dict):
            raise ValueError('Boundary must be an object')
        channel = boundary.get('channel')
        if (not isinstance(channel, list) or len(channel) != 3
                or not isinstance(channel[0], str)
                or type(channel[1]) is not int or type(channel[2]) is not int):
            raise ValueError('Invalid boundary channel')
        assigned.add(tuple(channel))
    generated_boundaries = []
    if default is not None:
        for key in all_keys:
            if key not in assigned and key not in original_keys and key[:2] not in connected:
                expanded['boundaries'].append(dict(copy.deepcopy(default), channel=list(key)))
                generated_boundaries.append(list(key))
    report = dict(method='native_structural_closure', total_channels=plan.total_channels,
                  generated_channels=plan.generated_channels, iterations=plan.iterations,
                  transitions=plan.transitions,
                  limits=dict(max_total_channels=options.get('max_total_channels', 512),
                              max_bin=options.get('max_bin', 2147483647),
                              max_transitions=options.get('max_transitions', 10000000)),
                  devices=[dict(id=entry['id'], seed_channel_indices=list(plan.seed_channel_indices[d]))
                           for d, entry in enumerate(entries)],
                  generated_boundary_channels=generated_boundaries)
    return expanded, plan.channels, report

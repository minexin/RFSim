"""Structural conversion-frequency planning through the native C++ implementation."""
import ctypes as ct
import math
from typing import NamedTuple


class ConversionFrequencyPlan(NamedTuple):
    channels: tuple
    seed_channel_indices: tuple
    total_channels: int
    generated_channels: int
    iterations: int
    transitions: int


class _FrequencyChannel(ct.Structure):
    _fields_ = [('port', ct.c_size_t), ('bin', ct.c_int)]


class _FrequencyRule(ct.Structure):
    _fields_ = [('kind', ct.c_int), ('input_port', ct.c_size_t),
                ('second_input_port', ct.c_size_t), ('output_port', ct.c_size_t),
                ('voltage_coefficients', ct.POINTER(ct.c_double)),
                ('coefficient_count', ct.c_size_t)]


class _HighOrderFrequencyRule(ct.Structure):
    _fields_ = [('device', ct.c_size_t), ('input_port', ct.c_size_t),
                ('output_port', ct.c_size_t), ('voltage_coefficients', ct.POINTER(ct.c_double)),
                ('coefficient_count', ct.c_size_t), ('generating_bins', ct.POINTER(ct.c_int)),
                ('generating_count', ct.c_size_t)]


class _FrequencyDevice(ct.Structure):
    _fields_ = [('ports', ct.POINTER(ct.c_size_t)), ('port_count', ct.c_size_t),
                ('seeds', ct.POINTER(_FrequencyChannel)), ('seed_count', ct.c_size_t),
                ('rules', ct.POINTER(_FrequencyRule)), ('rule_count', ct.c_size_t)]


class _FrequencyConnection(ct.Structure):
    _fields_ = [('first_device', ct.c_size_t), ('first_port', ct.c_size_t),
                ('second_device', ct.c_size_t), ('second_port', ct.c_size_t)]


class _FrequencyOptions(ct.Structure):
    _fields_ = [('max_total_channels', ct.c_size_t), ('max_bin', ct.c_int),
                ('max_transitions', ct.c_size_t)]


class _FrequencyRequest(ct.Structure):
    _fields_ = [('spacing_hz', ct.c_double), ('devices', ct.POINTER(_FrequencyDevice)),
                ('device_count', ct.c_size_t), ('connections', ct.POINTER(_FrequencyConnection)),
                ('connection_count', ct.c_size_t), ('options', ct.POINTER(_FrequencyOptions))]


class _FrequencyOutput(ct.Structure):
    _fields_ = [('channels', ct.POINTER(_FrequencyChannel)), ('channel_capacity', ct.c_size_t),
                ('device_offsets', ct.POINTER(ct.c_size_t)), ('device_offset_capacity', ct.c_size_t),
                ('seed_channel_indices', ct.POINTER(ct.c_size_t)), ('seed_capacity', ct.c_size_t)]


class _FrequencyDiagnostics(ct.Structure):
    _fields_ = [(name, ct.c_size_t) for name in
                ('device_count', 'seed_count', 'channel_count', 'generated_channels',
                 'iterations', 'transitions')]


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f'Expected integer in [{low}, {high}]')
    return value


def keys(value, required, optional=()):
    if not isinstance(value, dict) or set(required) - value.keys() or value.keys() - set(required) - set(optional):
        raise ValueError('Missing or unknown frequency-planning fields')


def plan(library, spacing_hz, devices, connections=(), *, max_total_channels=512,
         max_bin=2147483647, max_transitions=10000000):
    if type(spacing_hz) not in (int, float) or not math.isfinite(spacing_hz) or spacing_hz <= 0:
        raise ValueError('Frequency spacing must be finite and positive')
    options = _FrequencyOptions(integer(max_total_channels, 1, 512),
                                integer(max_bin, 0, 2147483647),
                                integer(max_transitions, 1, 10000000))
    entries = tuple(devices)
    if not 1 <= len(entries) <= 512:
        raise ValueError('Expected 1..512 frequency devices')
    native_devices, keepers, seed_counts, highorders = [], [], [], []
    for device_index, entry in enumerate(entries):
        keys(entry, ('ports',), ('seeds', 'rules'))
        ports = tuple(integer(p, 0, 1023) for p in entry['ports'])
        seeds = tuple((integer(p, 0, 1023), integer(b, 0, 2147483647))
                      for p, b in entry.get('seeds', ()))
        rules = tuple(entry.get('rules', ()))
        if not 1 <= len(ports) <= 512 or len(seeds) > 512 or len(rules) > 4096:
            raise ValueError('Frequency device dimensions exceed native bounds')
        native_rules = []
        for rule in rules:
            if not isinstance(rule, dict):
                raise ValueError('Frequency rule must be an object')
            kind = rule.get('type')
            if kind == 'copy':
                keys(rule, ('type', 'input_port', 'output_port'))
                value = _FrequencyRule(0, integer(rule['input_port'], 0, 1023), 0,
                                       integer(rule['output_port'], 0, 1023), None, 0)
            elif kind in ('polynomial', 'highorder'):
                required = ('type', 'input_port', 'output_port', 'voltage_coefficients')
                keys(rule, required + (('generating_bins',) if kind == 'highorder' else ()))
                coefficients = tuple(rule['voltage_coefficients'])
                if not 1 <= len(coefficients) <= 12 or any(
                    type(c) not in (int, float) or not math.isfinite(c) for c in coefficients
                ):
                    raise ValueError('Expected 1..12 finite polynomial coefficients')
                native_coefficients = (ct.c_double * len(coefficients))(*coefficients)
                keepers.append(native_coefficients)
                if kind == 'highorder':
                    generating = tuple(integer(b, 1, 2147483647) for b in rule['generating_bins'])
                    if len(generating) > 512:
                        raise ValueError('Expected at most 512 high-order generating bins')
                    native_generating = (ct.c_int * len(generating))(*generating)
                    keepers.append(native_generating)
                    highorders.append(_HighOrderFrequencyRule(
                        device_index, integer(rule['input_port'], 0, 1023),
                        integer(rule['output_port'], 0, 1023), native_coefficients,
                        len(coefficients), native_generating, len(generating)))
                    continue
                value = _FrequencyRule(1, integer(rule['input_port'], 0, 1023), 0,
                                       integer(rule['output_port'], 0, 1023),
                                       native_coefficients, len(coefficients))
            elif kind == 'mixer':
                keys(rule, ('type', 'rf_port', 'lo_port', 'if_port'))
                value = _FrequencyRule(2, integer(rule['rf_port'], 0, 1023),
                                       integer(rule['lo_port'], 0, 1023),
                                       integer(rule['if_port'], 0, 1023), None, 0)
            else:
                raise ValueError('Unknown frequency rule type')
            native_rules.append(value)
        native_ports = (ct.c_size_t * len(ports))(*ports)
        native_seeds = (_FrequencyChannel * len(seeds))(*(_FrequencyChannel(*s) for s in seeds))
        native_rules = (_FrequencyRule * len(native_rules))(*native_rules)
        keepers.append((native_ports, native_seeds, native_rules))
        seed_counts.append(len(seeds))
        native_devices.append(_FrequencyDevice(native_ports, len(ports), native_seeds,
                                                len(seeds), native_rules, len(native_rules)))
    pairs = []
    for first, second in connections:
        if (not isinstance(first, (list, tuple)) or len(first) != 2
                or not isinstance(second, (list, tuple)) or len(second) != 2):
            raise ValueError('Connections require two device/port pairs')
        pairs.append(_FrequencyConnection(integer(first[0], 0, len(entries)-1),
                                          integer(first[1], 0, 1023),
                                          integer(second[0], 0, len(entries)-1),
                                          integer(second[1], 0, 1023)))
    if len(pairs) > 256:
        raise ValueError('Too many frequency connections')
    native_devices = (_FrequencyDevice * len(entries))(*native_devices)
    native_connections = (_FrequencyConnection * len(pairs))(*pairs)
    request = _FrequencyRequest(spacing_hz, native_devices, len(entries), native_connections,
                                len(pairs), ct.pointer(options))
    diagnostics = _FrequencyDiagnostics()
    native_highorders = (_HighOrderFrequencyRule * len(highorders))(*highorders)
    if highorders:
        function = library._dll.rfmodel_plan_conversion_frequencies_with_highorders
        arguments = (ct.byref(request), native_highorders, len(highorders))
    else:
        function = library._dll.rfmodel_plan_conversion_frequencies
        arguments = (ct.byref(request),)
    library._check(function(*arguments, None, ct.byref(diagnostics)))
    channels = (_FrequencyChannel * diagnostics.channel_count)()
    offsets = (ct.c_size_t * (len(entries)+1))()
    indices = (ct.c_size_t * diagnostics.seed_count)()
    output = _FrequencyOutput(channels, len(channels), offsets, len(offsets), indices, len(indices))
    library._check(function(*arguments, ct.byref(output), ct.byref(diagnostics)))
    grids, mappings, position = [], [], 0
    for d, seed_count in enumerate(seed_counts):
        grids.append(tuple((channels[i].port, channels[i].bin) for i in range(offsets[d], offsets[d+1])))
        mappings.append(tuple(indices[position:position+seed_count]))
        position += seed_count
    return ConversionFrequencyPlan(tuple(grids), tuple(mappings), diagnostics.channel_count,
                                   diagnostics.generated_channels, diagnostics.iterations,
                                   diagnostics.transitions)

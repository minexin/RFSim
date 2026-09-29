"""Versioned linear-network JSON input; all numerical solving stays in the C++ core."""
import json
import math
from pathlib import Path


def _object(value, required, optional=()):
    if not isinstance(value, dict) or not set(required) <= value.keys():
        raise ValueError("Missing required object fields: " + ", ".join(required))
    if value.keys() - set(required) - set(optional):
        raise ValueError("Unknown object fields")


def _number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError("Expected a finite number")
    value = float(value)
    if not math.isfinite(value):
        raise ValueError("Expected a finite number")
    return value


def _complex(value):
    if isinstance(value, list) and len(value) == 2:
        return complex(_number(value[0]), _number(value[1]))
    return complex(_number(value))


def _matrix(value):
    if not isinstance(value, list) or not 1 <= len(value) <= 1024:
        raise ValueError("Expected a 1..1024 port matrix")
    if any(not isinstance(row, list) or len(row) != len(value) for row in value):
        raise ValueError("Matrix must be square")
    return [[_complex(entry) for entry in row] for row in value]


def _encode(matrix):
    return [[[value.real, value.imag] for value in row] for row in matrix]


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key: " + key)
        result[key] = value
    return result


def _parameter_samples(library, model, frequencies, reference):
    if not isinstance(model, dict):
        raise ValueError("model must be an object")
    kind = model.get("type")
    if kind == "transmission_line":
        _object(model, ("type", "characteristic_ohms", "delay_s"), ("propagation_loss_db",))
        evaluate = library.transmission_line
    elif kind == "rlgc_line":
        _object(model, ("type", "length_m"),
                ("resistance_ohms_per_m", "inductance_h_per_m", "conductance_s_per_m",
                 "capacitance_f_per_m"))
        evaluate = library.rlgc_line
    elif kind == "linear_amplifier":
        _object(model, ("type", "gain_db"),
                ("gain_phase_degrees", "reverse_isolation_db", "reverse_phase_degrees",
                 "input_impedance_ohms", "output_impedance_ohms"))
        evaluate = library.linear_amplifier
    else:
        raise ValueError("Unknown parameter model type")
    complex_fields = {"input_impedance_ohms", "output_impedance_ohms"}
    parameters = {key: (_complex(value) if key in complex_fields else _number(value))
                  for key, value in model.items() if key != "type"}
    return [evaluate(frequency, reference_ohms=reference, **parameters) for frequency in frequencies]


def load(path):
    """Read UTF-8 JSON, rejecting duplicate keys and nonstandard NaN/Infinity."""
    def invalid_constant(value):
        raise ValueError("Invalid JSON numeric constant: " + value)

    return json.loads(Path(path).read_text(encoding="utf-8"),
                      object_pairs_hook=_unique_object, parse_constant=invalid_constant)


def analyze(library, document):
    """Evaluate explicit frequency samples, returning JSON-compatible S/noise results."""
    _object(document, ("format", "version", "frequencies_hz", "devices", "external_ports"),
            ("reference_ohms", "connections", "terminations", "temperature_k",
             "intrinsic_noise_samples"))
    if (document["format"] != "rfmodel.linear-network" or
            type(document["version"]) is not int or document["version"] != 1):
        raise ValueError("Unsupported model format/version")
    frequencies = document["frequencies_hz"]
    if not isinstance(frequencies, list) or not 1 <= len(frequencies) <= 10000:
        raise ValueError("Expected 1..10000 frequency samples")
    frequencies = [_number(value) for value in frequencies]
    if frequencies[0] < 0 or any(a >= b for a, b in zip(frequencies, frequencies[1:])):
        raise ValueError("Frequencies must be nonnegative and strictly increasing")
    reference = _number(document.get("reference_ohms", 50.))
    if reference <= 0:
        raise ValueError("Reference impedance must be positive")
    if "temperature_k" in document and "intrinsic_noise_samples" in document:
        raise ValueError("Choose temperature or explicit intrinsic noise, not both")
    temperature = document.get("temperature_k")
    if "temperature_k" in document:
        temperature = _number(temperature)
        if temperature < 0:
            raise ValueError("Temperature must be nonnegative")
    noise_samples = document.get("intrinsic_noise_samples")
    if "intrinsic_noise_samples" in document:
        if not isinstance(noise_samples, list) or len(noise_samples) != len(frequencies):
            raise ValueError("One intrinsic noise matrix is required per frequency")

    devices = document["devices"]
    if not isinstance(devices, list) or not devices:
        raise ValueError("At least one device is required")
    offsets, prepared, total_ports = {}, [], 0
    for device in devices:
        _object(device, ("id",), ("s", "s_samples", "model"))
        name = device["id"]
        if not isinstance(name, str) or not name or name in offsets:
            raise ValueError("Device IDs must be unique nonempty strings")
        if sum(key in device for key in ("s", "s_samples", "model")) != 1:
            raise ValueError("Choose exactly one of s, s_samples or model")
        if "s" in device:
            matrices = [_matrix(device["s"])] * len(frequencies)
        elif "model" in device:
            matrices = _parameter_samples(library, device["model"], frequencies, reference)
        else:
            samples = device["s_samples"]
            if not isinstance(samples, list) or len(samples) != len(frequencies):
                raise ValueError("One S matrix is required per frequency")
            matrices = [_matrix(sample) for sample in samples]
        ports = len(matrices[0])
        if any(len(matrix) != ports for matrix in matrices) or total_ports + ports > 1024:
            raise ValueError("Port counts must be constant and total at most 1024")
        offsets[name] = (total_ports, ports)
        prepared.append((total_ports, matrices))
        total_ports += ports

    def endpoint(value):
        if not isinstance(value, list) or len(value) != 2 or not isinstance(value[0], str):
            raise ValueError("Endpoint must be [device_id, port_index]")
        name, port = value
        if name not in offsets or type(port) is not int or not 0 <= port < offsets[name][1]:
            raise ValueError("Unknown device or invalid local port")
        return offsets[name][0] + port

    connections = document.get("connections", [])
    terminations = document.get("terminations", [])
    externals = document["external_ports"]
    if not all(isinstance(value, list) for value in (connections, terminations, externals)):
        raise ValueError("Connections, terminations and external_ports must be arrays")
    selected = [endpoint(port) for port in externals]
    pairs = []
    for pair in connections:
        if not isinstance(pair, list) or len(pair) != 2:
            raise ValueError("Connection must contain two endpoints")
        pairs.append((endpoint(pair[0]), endpoint(pair[1])))
    boundaries = []
    for termination in terminations:
        _object(termination, ("port",), ("reflection",))
        boundaries.append((endpoint(termination["port"]),
                           _complex(termination.get("reflection", 0.))))

    results = []
    for index, frequency in enumerate(frequencies):
        with library.network(reference) as network:
            covariance = None
            if temperature is not None:
                covariance = [[0j] * total_ports for _ in range(total_ports)]
            for offset, matrices in prepared:
                matrix = matrices[index]
                network.add(matrix)
                if covariance is not None:
                    block = library.passive_noise(matrix, temperature)
                    for row in range(len(matrix)):
                        for column in range(len(matrix)):
                            covariance[offset + row][offset + column] = block[row][column]
            for first, second in pairs:
                network.connect(first, second)
            for port, reflection in boundaries:
                network.terminate(port, reflection=reflection)
            point = {"frequency_hz": frequency, "s": _encode(network.external_s(selected))}
            if noise_samples is not None:
                covariance = _matrix(noise_samples[index])
            if covariance is not None:
                point["noise_w_per_hz"] = _encode(network.external_noise(selected, covariance))
            results.append(point)
    return {"format": "rfmodel.linear-results", "version": 1,
            "reference_ohms": reference, "external_ports": externals, "samples": results}

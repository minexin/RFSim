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


def _model_path(value, base_directory=None):
    if not isinstance(value, str) or not value or "\x00" in value:
        raise ValueError("Touchstone path must be a nonempty string without NUL")
    path = Path(value)
    if not path.is_absolute() and base_directory is not None:
        path = Path(base_directory) / path
    return path.resolve()


def referenced_touchstone_paths(document, base_directory=None):
    """Collect data inputs for CLI overwrite protection, without opening files."""
    if not isinstance(document, dict):
        return set()
    networks = [document]
    if document.get("format") == "rfmodel.coherent-network":
        networks = [document.get("network", {})]
    if document.get("format") in ("rfmodel.spectrum-chain", "rfmodel.amplifier-components",
                                  "rfmodel.coherent-system"):
        field = "post_stages" if document.get("format") == "rfmodel.amplifier-components" else "stages"
        stages = document.get(field, [])
        networks = []
        if isinstance(stages, list):
            for stage in stages:
                if isinstance(stage, dict):
                    networks.append(stage.get("network", {}))
    paths = set()
    for network in networks:
        devices = network.get("devices", []) if isinstance(network, dict) else []
        if not isinstance(devices, list):
            continue
        for device in devices:
            model = device.get("model") if isinstance(device, dict) else None
            if isinstance(model, dict) and model.get("type") == "touchstone":
                paths.add(_model_path(model.get("path"), base_directory))
    return paths


def _touchstone_samples(library, model, frequencies, reference, base_directory, noise=None):
    _object(model, ("type", "path"), ("out_of_band",))
    if model["type"] != "touchstone":
        raise ValueError("Embedded noise requires a Touchstone model")
    temperature = 290.
    if noise is not None:
        _object(noise, ("touchstone",), ("reference_temperature_k",))
        if noise["touchstone"] is not True:
            raise ValueError("noise.touchstone must be true")
        temperature = _number(noise.get("reference_temperature_k", 290.))
    with library.touchstone(_model_path(model["path"], base_directory),
                            out_of_band=model.get("out_of_band", "reject")) as data:
        matrices = [data.s_parameters(frequency, reference_ohms=reference)
                    for frequency in frequencies]
        covariance = None
        if noise is not None:
            covariance = [data.noise_correlation(frequency, reference_ohms=reference,
                                               reference_temperature_k=temperature)
                          for frequency in frequencies]
    return matrices, covariance


def _parameter_samples(library, model, frequencies, reference, base_directory=None):
    if not isinstance(model, dict):
        raise ValueError("model must be an object")
    kind = model.get("type")
    if kind == "touchstone":
        return _touchstone_samples(library, model, frequencies, reference, base_directory)[0]
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


def _device_noise(library, specification, matrices):
    """Resolve independent device covariance; no implicit noiseless fallback."""
    _object(specification, (), ("temperature_k", "covariance", "covariance_samples", "noiseless"))
    if len(specification) != 1:
        raise ValueError("Device noise must select exactly one mode")
    ports = len(matrices[0])
    if "temperature_k" in specification:
        temperature = _number(specification["temperature_k"])
        return [library.passive_noise(matrix, temperature) for matrix in matrices]
    if "noiseless" in specification:
        if specification["noiseless"] is not True:
            raise ValueError("noiseless must be true")
        return [[[0j] * ports for _ in range(ports)]] * len(matrices)
    if "covariance" in specification:
        samples = [_matrix(specification["covariance"])] * len(matrices)
    else:
        values = specification["covariance_samples"]
        if not isinstance(values, list) or len(values) != len(matrices):
            raise ValueError("One device noise matrix is required per frequency")
        samples = [_matrix(value) for value in values]
    if any(len(sample) != ports for sample in samples):
        raise ValueError("Device noise dimensions must match its ports")
    return samples


def analyze(library, document, *, base_directory=None):
    """Evaluate explicit frequency samples, returning JSON-compatible S/noise results."""
    _object(document, ("format", "version", "frequencies_hz", "devices", "external_ports"),
            ("reference_ohms", "connections", "terminations", "temperature_k",
             "intrinsic_noise_samples", "signal_boundaries", "noise_boundaries"))
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
    device_noise_mode = any(isinstance(device, dict) and "noise" in device for device in devices)
    if device_noise_mode and (temperature is not None or noise_samples is not None):
        raise ValueError("Device noise cannot be mixed with top-level noise modes")
    offsets, prepared, total_ports = {}, [], 0
    for device in devices:
        _object(device, ("id",), ("s", "s_samples", "model", "noise"))
        if device_noise_mode and "noise" not in device:
            raise ValueError("Every device must explicitly specify noise in device-noise mode")
        name = device["id"]
        if not isinstance(name, str) or not name or name in offsets:
            raise ValueError("Device IDs must be unique nonempty strings")
        if sum(key in device for key in ("s", "s_samples", "model")) != 1:
            raise ValueError("Choose exactly one of s, s_samples or model")
        embedded_noise = isinstance(device.get("noise"), dict) and "touchstone" in device["noise"]
        device_noise = None
        if embedded_noise:
            matrices, device_noise = _touchstone_samples(
                library, device.get("model"), frequencies, reference, base_directory, device["noise"])
        elif "s" in device:
            matrices = [_matrix(device["s"])] * len(frequencies)
        elif "model" in device:
            matrices = _parameter_samples(library, device["model"], frequencies, reference,
                                          base_directory)
        else:
            samples = device["s_samples"]
            if not isinstance(samples, list) or len(samples) != len(frequencies):
                raise ValueError("One S matrix is required per frequency")
            matrices = [_matrix(sample) for sample in samples]
        ports = len(matrices[0])
        if any(len(matrix) != ports for matrix in matrices) or total_ports + ports > 1024:
            raise ValueError("Port counts must be constant and total at most 1024")
        offsets[name] = (total_ports, ports)
        if device_noise_mode and not embedded_noise:
            device_noise = _device_noise(library, device["noise"], matrices)
        prepared.append((total_ports, matrices, device_noise))
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

    signal_boundaries = None
    if "signal_boundaries" in document:
        entries = document["signal_boundaries"]
        if not isinstance(entries, list):
            raise ValueError("signal_boundaries must be an array")
        signal_boundaries = {}
        for entry in entries:
            _object(entry, ("port",), ("reflection", "source", "source_samples"))
            port = endpoint(entry["port"])
            if port not in selected or port in signal_boundaries:
                raise ValueError("Signal boundaries must uniquely cover external ports")
            if "source" in entry and "source_samples" in entry:
                raise ValueError("Choose source or source_samples")
            if "source_samples" in entry:
                samples = entry["source_samples"]
                if not isinstance(samples, list) or len(samples) != len(frequencies):
                    raise ValueError("One source amplitude is required per frequency")
                sources = [_complex(value) for value in samples]
            else:
                sources = [_complex(entry.get("source", 0.))] * len(frequencies)
            signal_boundaries[port] = (_complex(entry.get("reflection", 0.)), sources)
        if set(signal_boundaries) != set(selected):
            raise ValueError("Signal boundaries must specify every external port")

    noise_boundaries = None
    if "noise_boundaries" in document:
        if temperature is None and noise_samples is None and not device_noise_mode:
            raise ValueError("Noise boundaries require explicit intrinsic device noise")
        entries = document["noise_boundaries"]
        if not isinstance(entries, list):
            raise ValueError("noise_boundaries must be an array")
        noise_boundaries = {}
        for entry in entries:
            _object(entry, ("port", "reflection", "temperature_k"))
            port = endpoint(entry["port"])
            if port not in selected or port in noise_boundaries:
                raise ValueError("Noise boundaries must uniquely cover external ports")
            noise_boundaries[port] = (
                _complex(entry["reflection"]), _number(entry["temperature_k"]))
        if set(noise_boundaries) != set(selected):
            raise ValueError("Noise boundaries must specify every external port")
        reflections = [noise_boundaries[port][0] for port in selected]
        temperatures = [noise_boundaries[port][1] for port in selected]
        boundary_emission = library.thermal_boundary_noise(reflections, temperatures)

    results = []
    for index, frequency in enumerate(frequencies):
        with library.network(reference) as network:
            covariance = None
            if temperature is not None or device_noise_mode:
                covariance = [[0j] * total_ports for _ in range(total_ports)]
            for offset, matrices, device_noise in prepared:
                matrix = matrices[index]
                network.add(matrix)
                if covariance is not None:
                    block = (device_noise[index] if device_noise_mode
                             else library.passive_noise(matrix, temperature))
                    for row in range(len(matrix)):
                        for column in range(len(matrix)):
                            covariance[offset + row][offset + column] = block[row][column]
            for first, second in pairs:
                network.connect(first, second)
            for port, reflection in boundaries:
                network.terminate(port, reflection=reflection)
            scattering = network.external_s(selected)
            point = {"frequency_hz": frequency, "s": _encode(scattering)}
            if noise_samples is not None:
                covariance = _matrix(noise_samples[index])
            if covariance is not None:
                intrinsic = network.external_noise(selected, covariance)
                point["noise_w_per_hz"] = _encode(intrinsic)
                if noise_boundaries is not None:
                    loaded = library.loaded_noise(scattering, intrinsic, reflections,
                                                  boundary_emission)
                    point["loaded_noise"] = {
                        "incident_w_per_hz": _encode(loaded.incident),
                        "outgoing_w_per_hz": _encode(loaded.outgoing),
                        "net_into_device_w_per_hz": list(loaded.net_into_device_w_per_hz),
                    }
            if signal_boundaries is not None:
                for port, (reflection, sources) in signal_boundaries.items():
                    network.terminate(port, reflection=reflection, source=sources[index])
                waves = network.solve()
                port_waves = []
                for name, (offset, count) in offsets.items():
                    for local in range(count):
                        incident = waves.incident[offset + local]
                        outgoing = waves.outgoing[offset + local]
                        incident_power, outgoing_power = abs(incident)**2, abs(outgoing)**2
                        if not all(math.isfinite(value) for value in
                                   (incident_power, outgoing_power, incident_power-outgoing_power)):
                            raise ValueError("Signal port power overflow")
                        port_waves.append({"port": [name, local],
                                           "incident": [incident.real, incident.imag],
                                           "outgoing": [outgoing.real, outgoing.imag],
                                           "incident_power_w": incident_power,
                                           "outgoing_power_w": outgoing_power,
                                           "net_into_device_w": incident_power-outgoing_power})
                point["signal"] = {"ports": port_waves,
                                   "relative_residual": waves.relative_residual}
            results.append(point)
    return {"format": "rfmodel.linear-results", "version": 1,
            "reference_ohms": reference, "external_ports": externals, "samples": results}

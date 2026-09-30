"""Versioned matched forward spectrum chains; native kernels perform transmission."""
import math
from functools import partial
from .model_file import _object, _number, _complex, _matrix, analyze


def _index(value):
    if type(value) is not int or not 0 <= value <= 2147483647:
        raise ValueError("Spectrum bin must be a nonnegative signed 32-bit integer")
    return value


def _encode(amplitudes, spacing):
    result = []
    for index, amplitude in sorted(amplitudes.items()):
        power = abs(amplitude)**2
        frequency = index * spacing
        if not math.isfinite(power) or not math.isfinite(frequency):
            raise ValueError("Spectrum output power or frequency overflow")
        result.append({"bin": index, "frequency_hz": frequency,
                       "amplitude": [amplitude.real, amplitude.imag], "power_w": power})
    return result


def _linear_stage(library, spacing, amplitudes, *, reference_ohms, network, base_directory=None):
    """Evaluate the native network at every current bin, including new mixer products."""
    indices = sorted(amplitudes)
    document = dict(network, format="rfmodel.linear-network", version=1,
                    reference_ohms=reference_ohms,
                    frequencies_hz=[index * spacing for index in indices] or [0.])
    samples = analyze(library, document, base_directory=base_directory)["samples"]
    output = {}
    for index, sample in zip(indices, samples):
        # The reduced two-port includes all internal feedback at this frequency.
        # Native transmission enforces real DC and finite complex wave amplitudes.
        with library.network(reference_ohms) as reduced:
            reduced.add(_matrix(sample["s"]))
            output.update(reduced.transmit_spectrum(spacing, {index: amplitudes[index]}, [0, 1]))
    return output


def _p1db_stage(library, spacing, amplitudes, *, reference_ohms, power_gain_db, output_p1db_dbm):
    """One RF fundamental only; never apply a single-tone model independently to multiple tones."""
    tones = [(index, value) for index, value in amplitudes.items() if value != 0]
    if len(tones) > 1 or (tones and tones[0][0] == 0):
        raise ValueError("p1db_fundamental requires at most one nonzero RF tone and no DC")
    # Even empty input must validate the native model's parameter domain.
    index, amplitude = tones[0] if tones else (0, 0j)
    output = library.p1db_fundamental(amplitude, power_gain_db=power_gain_db,
                                    output_p1db_dbm=output_p1db_dbm)
    return {index: output} if output != 0 else {}


def analyze_spectrum(library, document, *, base_directory=None):
    _object(document, ("format", "version", "spacing_hz", "input", "stages"),
            ("reference_ohms",))
    if (document["format"] != "rfmodel.spectrum-chain" or
            type(document["version"]) is not int or document["version"] != 1):
        raise ValueError("Unsupported spectrum format/version")
    spacing = _number(document["spacing_hz"])
    reference = _number(document.get("reference_ohms", 50.))
    if spacing <= 0 or reference <= 0:
        raise ValueError("Spacing and reference impedance must be positive")
    entries = document["input"]
    if not isinstance(entries, list) or len(entries) > 2048:
        raise ValueError("Input must be an array of at most 2048 bins")
    amplitudes = {}
    for entry in entries:
        _object(entry, ("bin", "amplitude"))
        index, amplitude = _index(entry["bin"]), _complex(entry["amplitude"])
        if index in amplitudes or (index == 0 and amplitude.imag != 0):
            raise ValueError("Duplicate bin or nonreal DC")
        if not math.isfinite(index * spacing):
            raise ValueError("Input frequency overflow")
        amplitudes[index] = amplitude
    stages = document["stages"]
    if not isinstance(stages, list) or not 1 <= len(stages) <= 512:
        raise ValueError("Expected 1..512 stages")
    prepared, identifiers = [], set()
    for stage in stages:
        if not isinstance(stage, dict):
            raise ValueError("Stage must be an object")
        kind = stage.get("type")
        if kind == "cubic_amplifier":
            _object(stage, ("id", "type", "power_gain_db", "input_ip3_dbm"))
            parameters = {key: _number(stage[key]) for key in ("power_gain_db", "input_ip3_dbm")}
            operation = library.cubic_amplifier
        elif kind == "polynomial_amplifier":
            _object(stage, ("id", "type", "voltage_coefficients"))
            coefficients = stage["voltage_coefficients"]
            if not isinstance(coefficients, list) or not 1 <= len(coefficients) <= 10:
                raise ValueError("Expected 1..10 voltage coefficients")
            parameters = {"voltage_coefficients": [_number(value) for value in coefficients]}
            operation = library.polynomial_amplifier
        elif kind == "p1db_fundamental":
            _object(stage, ("id", "type", "power_gain_db", "output_p1db_dbm"))
            parameters = {key: _number(stage[key]) for key in ("power_gain_db", "output_p1db_dbm")}
            operation = partial(_p1db_stage, library)
        elif kind == "ideal_mixer":
            _object(stage, ("id", "type", "lo_bin"), ("conversion_gain_db", "lo_phase_radians"))
            parameters = {"lo_bin": _index(stage["lo_bin"]),
                          "conversion_gain_db": _number(stage.get("conversion_gain_db", 0.)),
                          "lo_phase_radians": _number(stage.get("lo_phase_radians", 0.))}
            if parameters["lo_bin"] == 0:
                raise ValueError("LO bin must be positive")
            operation = library.ideal_mixer
        elif kind == "linear_network":
            _object(stage, ("id", "type", "network"))
            network = stage["network"]
            _object(network, ("devices", "external_ports"), ("connections", "terminations"))
            if not isinstance(network["external_ports"], list) or len(network["external_ports"]) != 2:
                raise ValueError("Linear spectrum network requires two external ports")
            if not isinstance(network["devices"], list):
                raise ValueError("Linear spectrum devices must be an array")
            for device in network["devices"]:
                _object(device, ("id",), ("s", "model"))
            parameters = {"network": network}
            operation = partial(_linear_stage, library, base_directory=base_directory)
        else:
            raise ValueError("Unsupported spectrum stage type")
        identifier = stage["id"]
        if not isinstance(identifier, str) or not identifier or identifier in identifiers:
            raise ValueError("Stage IDs must be unique nonempty strings")
        identifiers.add(identifier)
        prepared.append((identifier, kind, operation, parameters))
    result = {"format": "rfmodel.spectrum-results", "version": 1,
              "spacing_hz": spacing, "reference_ohms": reference,
              "input": _encode(amplitudes, spacing), "stages": []}
    for identifier, kind, operation, parameters in prepared:
        amplitudes = operation(spacing, amplitudes, reference_ohms=reference, **parameters)
        result["stages"].append({"id": identifier, "type": kind,
                                 "spectrum": _encode(amplitudes, spacing)})
    return result

"""Versioned matched forward spectrum chains; native kernels perform transmission."""
import math
from .model_file import _object, _number, _complex


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


def analyze_spectrum(library, document):
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
        elif kind == "ideal_mixer":
            _object(stage, ("id", "type", "lo_bin"), ("conversion_gain_db", "lo_phase_radians"))
            parameters = {"lo_bin": _index(stage["lo_bin"]),
                          "conversion_gain_db": _number(stage.get("conversion_gain_db", 0.)),
                          "lo_phase_radians": _number(stage.get("lo_phase_radians", 0.))}
            if parameters["lo_bin"] == 0:
                raise ValueError("LO bin must be positive")
            operation = library.ideal_mixer
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

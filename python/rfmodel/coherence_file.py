"""Explicit deterministic RF coherence reduction; all summation is native."""
from . import CoherentComponent, SpectrumKind
from .model_file import _object, _number, _complex

KINDS = {"source": SpectrumKind.SOURCE, "harmonic": SpectrumKind.HARMONIC,
         "intermod": SpectrumKind.INTERMOD}


def _decode_component(value):
    _object(value, ("bin", "kind", "bandwidth_hz", "coherence_group", "amplitude"))
    if not isinstance(value["kind"], str) or value["kind"] not in KINDS:
        raise ValueError("Expected source, harmonic or intermod kind")
    return CoherentComponent(value["bin"], KINDS[value["kind"]], _number(value["bandwidth_hz"]),
                             value["coherence_group"], _complex(value["amplitude"]))


def _encode_reduction(result, spacing):
    names = {kind: name for name, kind in KINDS.items()}
    return {
        "components": [
            {"bin": c.bin, "kind": names[c.kind], "bandwidth_hz": c.bandwidth_hz,
             "coherence_group": c.coherence_group, "amplitude": [c.amplitude.real, c.amplitude.imag]}
            for c in result.components],
        "power_by_bin": [{"bin": index, "frequency_hz": index * spacing, "power_w": power}
                         for index, power in result.power_by_bin_w.items()],
        "total_power_w": result.total_power_w,
    }


def analyze_coherence(library, document, *, base_directory=None):
    _object(document, ("format", "version", "spacing_hz", "components"))
    if (document["format"] != "rfmodel.coherence" or
            type(document["version"]) is not int or document["version"] != 1):
        raise ValueError("Unsupported coherence format/version")
    spacing = _number(document["spacing_hz"])
    values = document["components"]
    if not isinstance(values, list) or len(values) > 4096:
        raise ValueError("Expected at most 4096 RF components")
    result = library.reduce_coherent_components(spacing, [_decode_component(value) for value in values])
    return dict(_encode_reduction(result, spacing),
                format="rfmodel.coherence-result", version=1, spacing_hz=spacing)

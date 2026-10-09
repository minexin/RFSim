"""Frequency-dependent connected networks with explicit RF coherence groups."""
from . import PortCoherentComponent
from .coherence_file import _decode_component, _encode_reduction
from .model_file import _object, _number, _matrix, analyze


def _endpoint(value):
    if (not isinstance(value, list) or len(value) != 2 or
            not isinstance(value[0], str) or not value[0] or
            type(value[1]) is not int or not 0 <= value[1] < 1024):
        raise ValueError("Endpoint must be [device_id, integer port_index]")
    return tuple(value)


def analyze_coherent_network(library, document, *, base_directory=None):
    _object(document, ("format", "version", "spacing_hz", "network", "inputs", "output_port"),
            ("reference_ohms",))
    if (document["format"] != "rfmodel.coherent-network" or
            type(document["version"]) is not int or document["version"] != 1):
        raise ValueError("Unsupported coherent network format/version")
    spacing = _number(document["spacing_hz"])
    reference = _number(document.get("reference_ohms", 50.))
    if spacing <= 0 or reference <= 0:
        raise ValueError("Spacing and reference impedance must be positive")
    network = document["network"]
    _object(network, ("devices", "external_ports"), ("connections", "terminations"))
    devices, external = network["devices"], network["external_ports"]
    if not isinstance(devices, list) or not devices:
        raise ValueError("At least one device is required")
    for device in devices:
        # The grid is derived from input bins. Explicit s_samples would be ambiguous here.
        _object(device, ("id",), ("s", "model"))
    if not isinstance(external, list) or not 1 <= len(external) <= 1024:
        raise ValueError("Select 1..1024 external ports")
    endpoints = [_endpoint(port) for port in external]
    if len(set(endpoints)) != len(endpoints):
        raise ValueError("Duplicate external port")
    positions = {port: index for index, port in enumerate(endpoints)}
    output = _endpoint(document["output_port"])
    if output not in positions:
        raise ValueError("Output must be a selected external port")
    values = document["inputs"]
    if not isinstance(values, list) or len(values) > 4096:
        raise ValueError("Expected at most 4096 port components")
    by_port = {}
    for value in values:
        _object(value, ("port", "component"))
        port = _endpoint(value["port"])
        if port not in positions:
            raise ValueError("Input must be a selected external port")
        by_port.setdefault(port, []).append(_decode_component(value["component"]))
    by_bin = {}
    for port, components in by_port.items():
        # Validate and merge only within one incident port before any frequency is sampled.
        validated = library.reduce_coherent_components(spacing, components)
        for component in validated.components:
            by_bin.setdefault(component.bin, []).append(
                PortCoherentComponent(positions[port], component))
    bins = sorted(by_bin)
    if len(bins) > 2048:
        raise ValueError("At most 2048 distinct frequency bins")
    linear = dict(network, format="rfmodel.linear-network", version=1, reference_ohms=reference,
                  frequencies_hz=[index * spacing for index in bins] or [0.])
    # Native external-S extraction includes all internal connections/reflections.
    samples = analyze(library, linear, base_directory=base_directory)["samples"]
    transmitted = []
    for index, sample in zip(bins or [0], samples):
        with library.network(reference) as reduced:
            reduced.add(_matrix(sample["s"]))
            result = reduced.transmit_coherent(
                spacing, by_bin.get(index, []), range(len(endpoints)), positions[output])
            transmitted.extend(result.components)
    result = library.reduce_coherent_components(spacing, transmitted)
    return dict(_encode_reduction(result, spacing),
                format="rfmodel.coherent-network-result", version=1, spacing_hz=spacing,
                reference_ohms=reference, external_ports=external,
                output_port=list(output))

"""Frequency-dependent connected networks with explicit RF coherence groups."""
from . import PortCoherentComponent, SourceCoherence
from .coherence_file import _decode_component, _encode_reduction
from .model_file import _object, _number, _matrix, analyze


def _endpoint(value):
    if (not isinstance(value, list) or len(value) != 2 or
            not isinstance(value[0], str) or not value[0] or
            type(value[1]) is not int or not 0 <= value[1] < 1024):
        raise ValueError("Endpoint must be [device_id, integer port_index]")
    return tuple(value)


def _resolve_sources(library, definitions):
    if not isinstance(definitions, list) or len(definitions) > 4096:
        raise ValueError("Expected at most 4096 source definitions")
    sources = []
    for definition in definitions:
        _object(definition, ("id",), ("reference_clock",))
        sources.append(SourceCoherence(definition["id"], definition.get("reference_clock", "")))
    assigned = library.assign_source_coherence(sources)
    groups = {source.source_id: group for source, group in zip(sources, assigned)}
    resolved = [
        {"id": source.source_id, "reference_clock": source.reference_clock, "coherence_group": group}
        for source, group in zip(sources, assigned)]
    return groups, resolved


def _source_component(value, groups):
    _object(value, ("source", "bin", "bandwidth_hz", "amplitude"))
    source = value["source"]
    if not isinstance(source, str) or source not in groups:
        raise ValueError("Unknown source definition")
    return _decode_component({
        "bin": value["bin"], "kind": "source", "bandwidth_hz": value["bandwidth_hz"],
        "amplitude": value["amplitude"], "coherence_group": groups[source]})


def _analyze_ports(library, spacing, reference, network, inputs, outputs, base_directory=None,
                   declared_inputs=()):
    """Extract the connected network once per frequency, then observe selected ports."""
    if spacing <= 0 or reference <= 0:
        raise ValueError("Spacing and reference impedance must be positive")
    _object(network, ("devices", "external_ports"), ("connections", "terminations"))
    devices, external = network["devices"], network["external_ports"]
    if not isinstance(devices, list) or not devices:
        raise ValueError("At least one device is required")
    for device in devices:
        _object(device, ("id",), ("s", "model"))
    if not isinstance(external, list) or not 1 <= len(external) <= 1024:
        raise ValueError("Select 1..1024 external ports")
    endpoints = [_endpoint(port) for port in external]
    if len(set(endpoints)) != len(endpoints):
        raise ValueError("Duplicate external port")
    positions = {port: index for index, port in enumerate(endpoints)}
    if not outputs or len(outputs) > 1024 or len(set(outputs)) != len(outputs):
        raise ValueError("Select 1..1024 unique output ports")
    if any(output not in positions for output in outputs):
        raise ValueError("Output must be a selected external port")
    if len(inputs) > 4096 or len(inputs) * len(outputs) > 65536:
        raise ValueError("Coherent network component/output work limit exceeded")
    if any(port not in positions for port in declared_inputs):
        raise ValueError("Input must be a selected external port")
    by_port = {}
    for port, component in inputs:
        if port not in positions:
            raise ValueError("Input must be a selected external port")
        by_port.setdefault(port, []).append(component)
    by_bin = {}
    for port, components in by_port.items():
        validated = library.reduce_coherent_components(spacing, components)
        for component in validated.components:
            by_bin.setdefault(component.bin, []).append(
                PortCoherentComponent(positions[port], component))
    bins = sorted(by_bin)
    if len(bins) > 2048:
        raise ValueError("At most 2048 distinct frequency bins")
    linear = dict(network, format="rfmodel.linear-network", version=1, reference_ohms=reference,
                  frequencies_hz=[index * spacing for index in bins] or [0.])
    samples = analyze(library, linear, base_directory=base_directory)["samples"]
    transmitted = {output: [] for output in outputs}
    for index, sample in zip(bins or [0], samples):
        with library.network(reference) as reduced:
            reduced.add(_matrix(sample["s"]))
            for output in outputs:
                result = reduced.transmit_coherent(
                    spacing, by_bin.get(index, []), range(len(endpoints)), positions[output])
                transmitted[output].extend(result.components)
    return {output: library.reduce_coherent_components(spacing, components)
            for output, components in transmitted.items()}


def analyze_coherent_network(library, document, *, base_directory=None):
    _object(document, ("format", "version", "spacing_hz", "network", "inputs", "output_port"),
            ("reference_ohms", "sources"))
    if (document["format"] != "rfmodel.coherent-network" or
            type(document["version"]) is not int or document["version"] != 1):
        raise ValueError("Unsupported coherent network format/version")
    spacing = _number(document["spacing_hz"])
    reference = _number(document.get("reference_ohms", 50.))
    values = document["inputs"]
    if not isinstance(values, list) or len(values) > 4096:
        raise ValueError("Expected at most 4096 port components")
    source_groups, resolved_sources = None, []
    if "sources" in document:
        source_groups, resolved_sources = _resolve_sources(library, document["sources"])
    inputs = []
    for value in values:
        _object(value, ("port", "component"))
        component = (_source_component(value["component"], source_groups)
                     if source_groups is not None else _decode_component(value["component"]))
        inputs.append((_endpoint(value["port"]), component))
    output = _endpoint(document["output_port"])
    result = _analyze_ports(library, spacing, reference, document["network"], inputs,
                            [output], base_directory)[output]
    encoded = dict(_encode_reduction(result, spacing),
                   format="rfmodel.coherent-network-result", version=1, spacing_hz=spacing,
                   reference_ohms=reference, external_ports=document["network"]["external_ports"],
                   output_port=list(output))
    if source_groups is not None:
        encoded["sources"] = resolved_sources
    return encoded

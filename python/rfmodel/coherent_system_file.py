"""Feed-forward coherent RF graphs: linear networks and prescribed CW mixer banks."""
from . import CoherentMixerInput
from .coherence_file import _encode_reduction
from .coherent_network_file import (
    _analyze_ports, _endpoint, _resolve_sources, _source_component,
)
from .model_file import _number, _object


def _label(value):
    if (not isinstance(value, str) or not value or chr(0) in value or
            len(value.encode("utf-8")) > 1024):
        raise ValueError("Expected a nonempty label of at most 1024 UTF-8 bytes")
    return value


def _array(value, maximum, *, nonempty=False):
    if not isinstance(value, list) or len(value) > maximum or (nonempty and not value):
        raise ValueError("Invalid graph array size")
    return value


def analyze_coherent_system(library, document, *, base_directory=None):
    _object(document, ("format", "version", "spacing_hz", "sources", "inputs", "stages", "outputs"),
            ("reference_ohms",))
    if (document["format"] != "rfmodel.coherent-system" or
            type(document["version"]) is not int or document["version"] != 1):
        raise ValueError("Unsupported coherent system format/version")
    spacing = _number(document["spacing_hz"])
    reference = _number(document.get("reference_ohms", 50.))
    if spacing <= 0 or reference <= 0:
        raise ValueError("Spacing and reference impedance must be positive")
    groups, resolved_sources = _resolve_sources(library, document["sources"])
    highest_group = max(groups.values(), default=0)
    streams = {}
    stored_components = 0
    stage_ids, stage_results = set(), []
    # Native mixer batches provide local groups; this per-analysis registry aliases
    # repeated (RF, LO) identities across stages to their first allocated group.
    mixed_groups = {}

    def new_ids(values):
        names = [_label(value) for value in values]
        if len(set(names)) != len(names) or any(name in streams for name in names):
            raise ValueError("Duplicate stream ID")
        if len(streams) + len(names) > 4096:
            raise ValueError("At most 4096 streams")
        return names

    def read_stream(name):
        name = _label(name)
        if name not in streams:
            raise ValueError("Unknown or forward-referenced stream: " + name)
        return streams[name].components

    def store(name, result):
        nonlocal stored_components
        stored_components += len(result.components)
        if stored_components > 65536:
            raise ValueError("Stored graph components exceed 65536")
        streams[name] = result

    inputs = _array(document["inputs"], 4096)
    new_ids([value.get("id") if isinstance(value, dict) else None for value in inputs])
    for value in inputs:
        _object(value, ("id", "components"))
        components = [_source_component(c, groups)
                      for c in _array(value["components"], 4096)]
        store(value["id"], library.reduce_coherent_components(spacing, components))

    for stage in _array(document["stages"], 512):
        if not isinstance(stage, dict):
            raise ValueError("Stage must be an object")
        name = _label(stage.get("id"))
        if name in stage_ids:
            raise ValueError("Duplicate stage ID")
        stage_ids.add(name)
        kind = stage.get("type")
        if kind == "linear_network":
            _object(stage, ("id", "type", "network", "inputs", "outputs"))
            values = _array(stage["outputs"], 1024, nonempty=True)
            for value in values:
                _object(value, ("id", "port"))
            names = new_ids([value["id"] for value in values])
            ports = [_endpoint(value["port"]) for value in values]
            incident, declared_ports = [], []
            for value in _array(stage["inputs"], 4096):
                _object(value, ("stream", "port"))
                port = _endpoint(value["port"])
                declared_ports.append(port)
                incident.extend((port, c) for c in read_stream(value["stream"]))
                if len(incident) > 4096:
                    raise ValueError("At most 4096 incident port components")
            results = _analyze_ports(library, spacing, reference, stage["network"],
                                     incident, ports, base_directory, declared_inputs=declared_ports)
            for output_name, port in zip(names, ports):
                store(output_name, results[port])
        elif kind == "ideal_mixer_bank":
            _object(stage, ("id", "type", "branches"))
            branches = _array(stage["branches"], 2048, nonempty=True)
            for branch in branches:
                _object(branch, ("id", "input", "lo_source", "lo_bin", "conversion_gain_db"),
                        ("lo_phase_radians",))
            names = new_ids([branch["id"] for branch in branches])
            incident, sizes = [], []
            for branch in branches:
                source = _label(branch["lo_source"])
                if source not in groups:
                    raise ValueError("Unknown LO source")
                gain = _number(branch["conversion_gain_db"])
                phase = _number(branch.get("lo_phase_radians", 0.))
                # Even a silent branch must have valid LO and conversion parameters.
                library.ideal_mixer(spacing, {}, lo_bin=branch["lo_bin"], conversion_gain_db=gain,
                                    lo_phase_radians=phase, reference_ohms=reference)
                components = read_stream(branch["input"])
                sizes.append(len(components))
                incident.extend(CoherentMixerInput(c, branch["lo_bin"], gain, phase, groups[source])
                                for c in components)
                if len(incident) > 2048:
                    raise ValueError("Mixer bank accepts at most 2048 RF components")
            converted = library.mix_coherent_components(
                spacing, incident, reserved_group_max=highest_group)
            remapped = []
            for index, value in enumerate(incident):
                local = converted[2 * index].coherence_group
                highest_group = max(highest_group, local)
                key = (value.component.coherence_group, value.lo_coherence_group)
                shared = mixed_groups.setdefault(key, local)
                remapped.extend(c._replace(coherence_group=shared)
                                for c in converted[2 * index:2 * index + 2])
            offset = 0
            for output_name, count in zip(names, sizes):
                stop = offset + 2 * count
                store(output_name, library.reduce_coherent_components(spacing, remapped[offset:stop]))
                offset = stop
        else:
            raise ValueError("Expected linear_network or ideal_mixer_bank stage")
        stage_results.append({"id": name, "type": kind, "outputs": names})

    outputs = [_label(name) for name in _array(document["outputs"], 4096, nonempty=True)]
    if len(set(outputs)) != len(outputs):
        raise ValueError("Duplicate requested output")
    for name in outputs:
        read_stream(name)
    return {"format": "rfmodel.coherent-system-result", "version": 1,
            "spacing_hz": spacing, "reference_ohms": reference,
            "sources": resolved_sources, "stages": stage_results, "outputs": outputs,
            "streams": [{"id": name, **_encode_reduction(result, spacing)}
                        for name, result in streams.items()]}

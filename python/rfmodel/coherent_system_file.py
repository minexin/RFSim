"""Feed-forward coherent RF graphs with networks, mixers and nonlinear amplifiers."""
from . import CoherentMixerInput, SpectrumKind
from .coherent_origins import OriginRegistry
from .coherent_graph_support import _array, _label
from .coherence_file import _encode_reduction
from .coherent_network_file import (
    _analyze_ports, _endpoint, _resolve_sources, _source_component,
)
from .model_file import _number, _object


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
    stages = _array(document["stages"], 512)
    if any(isinstance(stage, dict) and stage.get("type") == "highorder_amplifier"
           for stage in stages):
        from .expression_system import analyze_expression_system
        return analyze_expression_system(library, document, base_directory=base_directory)
    trace_origins = any(isinstance(stage, dict) and stage.get("type") == "polynomial_amplifier"
                        for stage in stages)
    if trace_origins and any(isinstance(stage, dict) and stage.get("type") == "ideal_mixer_bank"
                             for stage in stages):
        from .expression_system import analyze_expression_system
        return analyze_expression_system(library, document, base_directory=base_directory)
    groups, resolved_sources = _resolve_sources(library, document["sources"])
    highest_group = max(groups.values(), default=0)
    lineage = OriginRegistry(library, highest_group)
    streams = {}
    stored_components = 0
    stage_ids, stage_results = set(), []
    # Native mixer batches provide local groups; this per-analysis registry aliases
    # repeated (RF, LO) identities across stages to their first allocated group.
    mixed_groups = {}
    amplifier_groups = {}

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
        reduced = library.reduce_coherent_components(spacing, components)
        if trace_origins:
            for component in reduced.components:
                lineage.seed(component)
        store(value["id"], reduced)

    for stage in stages:
        if not isinstance(stage, dict):
            raise ValueError("Stage must be an object")
        name = _label(stage.get("id"))
        if name in stage_ids:
            raise ValueError("Duplicate stage ID")
        stage_ids.add(name)
        kind = stage.get("type")
        measurements = {}
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
        elif kind in ("limited_amplifier", "cascaded_amplifier"):
            _object(stage, ("id", "type", "input", "output", "power_gain_db",
                            "output_p1db_dbm", "output_saturation_dbm",
                            "input_ip2_dbm", "input_ip3_dbm"))
            names = new_ids([stage["output"]])
            result = library.coherent_amplifier(
                spacing, read_stream(stage["input"]), reference_ohms=reference,
                propagate_distortion=kind == "cascaded_amplifier",
                reserved_group_max=highest_group,
                **{key: _number(stage[key]) for key in (
                    "power_gain_db", "output_p1db_dbm", "output_saturation_dbm",
                    "input_ip2_dbm", "input_ip3_dbm")})
            parents = [lineage.lookup(component) for component in result.inputs] if trace_origins else []
            remapped, origins = [], []
            for term in result.terms:
                c = term.component
                if term.order > 1:
                    # Canonical signed multiset, independent of the local input
                    # array and amplifier ID. Do not cancel conjugate pairs.
                    contributors = []
                    for index in term.input_indices:
                        parent = result.inputs[abs(index) - 1]
                        contributors.append((1 if index > 0 else -1, parent.bin,
                                             int(parent.kind), parent.bandwidth_hz,
                                             parent.coherence_group))
                    key = tuple(sorted(contributors))
                    highest_group = max(highest_group, c.coherence_group)
                    shared = amplifier_groups.setdefault(key, c.coherence_group)
                    c = c._replace(coherence_group=shared)
                if trace_origins:
                    lineage.highest_group = max(lineage.highest_group, highest_group)
                    c = lineage.register(c, lineage.compose(parents, term.input_indices))
                    highest_group = max(highest_group, lineage.highest_group)
                remapped.append(c)
                origins.append({"order": term.order, "input_indices": list(term.input_indices),
                                "bin": c.bin, "coherence_group": c.coherence_group})
                if trace_origins:
                    origins[-1].update(lineage.encode(c))
            store(names[0], library.reduce_coherent_components(spacing, remapped))
            measurements.update(
                input_power_w=result.total_input_power_w,
                limited_input_power_w=result.limited_input_power_w,
                reduced_inputs=[
                    {"bin": c.bin, "kind": c.kind.name.lower(), "bandwidth_hz": c.bandwidth_hz,
                     "coherence_group": c.coherence_group,
                     "amplitude": [c.amplitude.real, c.amplitude.imag]}
                    for c in result.inputs],
                origins=origins)
        elif kind == "polynomial_amplifier":
            _object(stage, ("id", "type", "input", "output", "voltage_coefficients", "max_source_order"))
            maximum = stage["max_source_order"]
            if type(maximum) is not int or not 1 <= maximum <= 256:
                raise ValueError("max_source_order must be an integer from 1 to 256")
            coefficients = [_number(value) for value in
                            _array(stage["voltage_coefficients"], 12, nonempty=True)]
            names = new_ids([stage["output"]])
            lineage.highest_group = highest_group
            result = library.coherent_polynomial(
                spacing, read_stream(stage["input"]), coefficients, reference_ohms=reference,
                reserved_group_max=lineage.highest_group)
            lineage.highest_group = max(
                [lineage.highest_group] + [term.component.coherence_group for term in result.terms])
            parents = [lineage.lookup(component) for component in result.inputs]
            remapped, origins, discarded = [], [], {}
            for term in result.terms:
                source_order = sum(len(parents[abs(index) - 1]) for index in term.input_indices)
                if source_order > maximum:
                    discarded[source_order] = discarded.get(source_order, 0) + 1
                    continue
                expanded = lineage.compose(parents, term.input_indices)
                component = term.component
                if term.order > 1:
                    harmonic = all(factor.sign == 1 and factor.root_id == expanded[0].root_id
                                   for factor in expanded)
                    component = component._replace(
                        kind=SpectrumKind.HARMONIC if harmonic else SpectrumKind.INTERMOD)
                component = lineage.register(component, expanded)
                remapped.append(component)
                origins.append({"order": term.order, "input_indices": list(term.input_indices),
                                **lineage.encode(component)})
            highest_group = lineage.highest_group
            store(names[0], library.reduce_coherent_components(spacing, remapped))
            measurements.update(
                max_source_order=maximum, generated_term_count=len(result.terms),
                discarded_term_count=sum(discarded.values()),
                discarded_by_source_order=[{"source_order": order, "term_count": count}
                                           for order, count in sorted(discarded.items())],
                reduced_inputs=[lineage.encode(component) for component in result.inputs],
                origins=origins)
        elif kind == "fundamental_compression":
            _object(stage, ("id", "type", "input", "output", "power_gain_db",
                            "output_p1db_dbm", "output_saturation_dbm"))
            names = new_ids([stage["output"]])
            result = library.compress_coherent_fundamentals(
                spacing, read_stream(stage["input"]),
                power_gain_db=_number(stage["power_gain_db"]),
                output_p1db_dbm=_number(stage["output_p1db_dbm"]),
                output_saturation_dbm=_number(stage["output_saturation_dbm"]))
            store(names[0], result.output)
            measurements["input_power_w"] = result.input_power_w
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
            raise ValueError(
                "Expected linear_network, ideal_mixer_bank, fundamental_compression, "
                "limited_amplifier, cascaded_amplifier or polynomial_amplifier stage")
        stage_results.append({"id": name, "type": kind, "outputs": names, **measurements})

    outputs = [_label(name) for name in _array(document["outputs"], 4096, nonempty=True)]
    if len(set(outputs)) != len(outputs):
        raise ValueError("Duplicate requested output")
    for name in outputs:
        read_stream(name)
    result = {"format": "rfmodel.coherent-system-result", "version": 1,
            "spacing_hz": spacing, "reference_ohms": reference,
            "sources": resolved_sources, "stages": stage_results, "outputs": outputs,
            "streams": [{"id": name, **_encode_reduction(result, spacing)}
                        for name, result in streams.items()]}

    if trace_origins:
        result["origin_roots"] = lineage.roots
        for encoded, value in zip(result["streams"], streams.values()):
            encoded["origins"] = [lineage.encode(component) for component in value.components]
    return result

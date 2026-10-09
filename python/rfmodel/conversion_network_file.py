"""Native multi-device frequency-conversion network assembly and linear lifting."""

from .model_file import (
    _object,
    _number,
    _complex,
    _matrix,
    _encode,
    _parameter_samples,
    _device_noise,
    _touchstone_samples,
)
from .operating_point import (
    bilinear_spec,
    amplifier_spec,
    operating_options,
    update_converged_models,
)
from .affine_conversion import offset_vector, linearized_mixer, check_operating_points
from .conversion_file import conversion_matrices
from .phase_noise import apply_phase_noise_sources, build_phase_noise_groups
from .channel_measurements import parse_channel_measurements, measure_conversion_channels


def zero(n):
    return [[0j for _ in range(n)] for _ in range(n)]


def noise_pair(specification, count):
    if specification == {"noiseless": True} and specification.get("noiseless") is True:
        return zero(count), zero(count)
    _object(specification, ("covariance",), ("complementary",))
    c = _matrix(specification["covariance"])
    p = _matrix(specification["complementary"]) if "complementary" in specification else zero(count)
    if len(c) != count or len(p) != count:
        raise ValueError("Conversion noise dimensions differ")
    return c, p


def linear_device(library, device, spacing, reference, base_directory):
    _object(device, ("id", "bins", "model", "noise"), ("source_noise", "output_offset"))
    bins = device["bins"]
    if (
        not isinstance(bins, list)
        or not 1 <= len(bins) <= 512
        or any(type(b) is not int or not 0 <= b <= 2147483647 for b in bins)
    ):
        raise ValueError("Expected 1..512 nonnegative integer linear bins")
    if any(a >= b for a, b in zip(bins, bins[1:])):
        raise ValueError("Linear bins must strictly increase")
    model = device["model"]
    _object(model, ("type",), ("device", "s"))
    if ("device" in model) == ("s" in model):
        raise ValueError("Linear conversion device requires one parameter model or static S matrix")
    frequencies = [b * spacing for b in bins]
    specification = device["noise"]
    if (
        "device" in model
        and isinstance(model["device"], dict)
        and model["device"].get("type") == "touchstone"
        and isinstance(specification, dict)
        and "touchstone" in specification
    ):
        samples, covariance = _touchstone_samples(
            library, model["device"], frequencies, reference, base_directory, specification
        )
    else:
        samples = (
            [_matrix(model["s"])] * len(bins)
            if "s" in model
            else _parameter_samples(
                library, model["device"], frequencies, reference, base_directory
            )
        )
        covariance = _device_noise(library, specification, samples)
    ports = len(samples[0])
    count = ports * len(bins)
    if count > 512 or any(len(sample) != ports for sample in samples):
        raise ValueError("Invalid lifted linear channel count")
    direct, c, p = zero(count), zero(count), zero(count)
    channels = []
    for k, index in enumerate(bins):
        for i in range(ports):
            channels.append((i, index))
            for j in range(ports):
                direct[k * ports + i][k * ports + j] = samples[k][i][j]
                c[k * ports + i][k * ports + j] = covariance[k][i][j]
                if index == 0:
                    p[k * ports + i][k * ports + j] = covariance[k][i][j]
    return {
        "channels": channels,
        "direct": direct,
        "intrinsic_covariance": c,
        "intrinsic_complementary": p,
    }


def analyze_conversion_network(library, document, base_directory=None):
    _object(
        document,
        ("format", "version", "spacing_hz", "devices", "boundaries"),
        (
            "reference_ohms",
            "connections",
            "noise_analyses",
            "loaded_noise",
            "channel_measurements",
            "phase_noise_sources",
            "phase_noise_groups",
            "additional_source_noise",
            "operating_point",
        ),
    )
    if (
        document["format"] != "rfmodel.conversion-network"
        or type(document["version"]) is not int
        or document["version"] != 1
    ):
        raise ValueError("Unsupported conversion-network format/version")
    spacing = _number(document["spacing_hz"])
    reference = _number(document.get("reference_ohms", 50.0))
    if spacing <= 0 or reference <= 0:
        raise ValueError("Conversion spacing and reference must be positive")
    entries = document["devices"]
    if not isinstance(entries, list) or not 1 <= len(entries) <= 512:
        raise ValueError("Expected 1..512 conversion devices")
    devices, names, labels, lookup = [], {}, [], {}
    output_offsets, operating_points = [], []
    use_affine = False
    nonlinear_mixers, nonlinear_amplifiers = [], []
    for entry in entries:
        if (
            not isinstance(entry, dict)
            or not isinstance(entry.get("id"), str)
            or not entry["id"]
            or entry["id"] in names
        ):
            raise ValueError("Conversion device IDs must be unique nonempty strings")
        name = entry["id"]
        model = entry.get("model")
        generated_offset = None
        if isinstance(model, dict) and model.get("type") == "linear":
            native = linear_device(library, entry, spacing, reference, base_directory)
        else:
            _object(entry, ("id", "channels", "model", "noise"), ("source_noise", "output_offset"))
            if not isinstance(entry["channels"], list) or not 1 <= len(entry["channels"]) <= 512:
                raise ValueError("Expected conversion channel list")
            channels = []
            for channel in entry["channels"]:
                _object(channel, ("port", "bin"))
                if type(channel["port"]) is not int or type(channel["bin"]) is not int:
                    raise ValueError("Conversion indices must be integers")
                channels.append((channel["port"], channel["bin"]))
            if isinstance(model, dict) and model.get("type") == "saturating_amplifier":
                if "operating_point" not in document:
                    raise ValueError(
                        "Saturating amplifiers require explicit operating_point options"
                    )
                nonlinear_amplifiers.append(dict(device=len(devices), **amplifier_spec(model)))
                a, b = zero(len(channels)), zero(len(channels))
            elif isinstance(model, dict) and model.get("type") == "bilinear_real_mixer":
                if "operating_point" not in document:
                    raise ValueError("Bilinear mixers require explicit operating_point options")
                nonlinear_mixers.append(dict(device=len(devices), **bilinear_spec(model)))
                a, b = zero(len(channels)), zero(len(channels))
            elif isinstance(model, dict) and model.get("type") == "linearized_real_mixer":
                if "output_offset" in entry:
                    raise ValueError("Linearized mixer computes its own output offset")
                lin, operating = linearized_mixer(library, spacing, channels, model, reference)
                a, b = lin.direct, lin.conjugate
                generated_offset = list(lin.output_offset)
                operating_points.append((name, len(labels), operating))
            else:
                a, b = conversion_matrices(library, spacing, channels, model, reference)
            c, p = noise_pair(entry["noise"], len(channels))
            native = {
                "channels": channels,
                "direct": a,
                "conjugate": b,
                "intrinsic_covariance": c,
                "intrinsic_complementary": p,
            }
        count = len(native["channels"])
        if len(labels) + count > 512:
            raise ValueError("Conversion network exceeds 512 channels")
        if generated_offset is not None:
            output_offsets.extend(generated_offset)
            use_affine = True
        elif "output_offset" in entry:
            output_offsets.extend(offset_vector(entry["output_offset"], count))
            use_affine = True
        else:
            output_offsets.extend([0j] * count)
        native["source"] = [0j] * count
        native["reflection"] = [0j] * count
        if "source_noise" in entry:
            c, p = noise_pair(entry["source_noise"], count)
        else:
            c, p = zero(count), zero(count)
        native["source_covariance"] = c
        native["source_complementary"] = p
        device_index = len(devices)
        names[name] = device_index
        for local, (port, index) in enumerate(native["channels"]):
            key = (name, port, index)
            if key in lookup:
                raise ValueError("Duplicate conversion channel")
            lookup[key] = (device_index, local)
            labels.append(key)
        devices.append(native)
    wires = document.get("connections", [])
    if not isinstance(wires, list):
        raise ValueError("Connections must be a list")
    connections, connected_ports = [], set()
    for wire in wires:
        if not isinstance(wire, list) or len(wire) != 2:
            raise ValueError("A connection requires two physical port endpoints")
        endpoints = []
        for endpoint in wire:
            if (
                not isinstance(endpoint, list)
                or len(endpoint) != 2
                or not isinstance(endpoint[0], str)
                or endpoint[0] not in names
                or type(endpoint[1]) is not int
            ):
                raise ValueError("Invalid conversion connection endpoint")
            endpoints.append((names[endpoint[0]], endpoint[1]))
            connected_ports.add(tuple(endpoint))
        connections.append(tuple(endpoints))
    boundaries = document["boundaries"]
    if not isinstance(boundaries, list):
        raise ValueError("Boundaries must be a list")
    loaded_noise = document.get("loaded_noise", False)
    if type(loaded_noise) is not bool:
        raise ValueError("loaded_noise must be boolean")
    assigned = set()
    for boundary in boundaries:
        _object(
            boundary,
            ("channel",),
            ("source", "reflection", "noise_w_per_hz", "noise_temperature_k"),
        )
        channel = boundary["channel"]
        if (
            not isinstance(channel, list)
            or len(channel) != 3
            or not isinstance(channel[0], str)
            or type(channel[1]) is not int
            or type(channel[2]) is not int
        ):
            raise ValueError("Invalid conversion boundary channel")
        key = tuple(channel)
        if key not in lookup or key in assigned or key[:2] in connected_ports:
            raise ValueError("Unknown, repeated or connected boundary channel")
        assigned.add(key)
        d, i = lookup[key]
        native = devices[d]
        native["source"][i] = _complex(boundary.get("source", 0))
        native["reflection"][i] = _complex(boundary.get("reflection", 0))
        noise_fields = {"noise_w_per_hz", "noise_temperature_k"} & boundary.keys()
        if noise_fields:
            if len(noise_fields) != 1 or "source_noise" in entries[d]:
                raise ValueError("Choose device source_noise or one boundary noise specification")
            if "noise_w_per_hz" in boundary:
                variance = _number(boundary["noise_w_per_hz"])
            else:
                temperature = _number(boundary["noise_temperature_k"])
                gamma = native["reflection"][i]
                if temperature < 0 or abs(gamma) > 1:
                    raise ValueError(
                        "Thermal boundary needs nonnegative temperature and passive reflection"
                    )
                variance = 1.380649e-23 * temperature * max(0.0, 1.0 - abs(gamma) ** 2)
            if variance < 0:
                raise ValueError("Boundary noise density must be nonnegative")
            native["source_covariance"][i][i] = variance
            if key[2] == 0:
                native["source_complementary"][i][i] = variance
    required = {key for key in labels if key[:2] not in connected_ports}
    if assigned != required:
        raise ValueError("Every unconnected channel requires one explicit boundary")
    phase_noise_sources = (
        apply_phase_noise_sources(
            library, document["phase_noise_sources"], devices, lookup, connected_ports
        )
        if "phase_noise_sources" in document
        else []
    )
    extra_c = extra_p = None
    phase_noise_groups = []
    if "phase_noise_groups" in document:
        if "additional_source_noise" in document:
            raise ValueError("Choose phase_noise_groups or explicit additional_source_noise")
        phase_noise_groups, extra_c, extra_p = build_phase_noise_groups(
            library, document["phase_noise_groups"], devices, labels, lookup, connected_ports
        )
    elif "additional_source_noise" in document:
        extra_c, extra_p = noise_pair(document["additional_source_noise"], len(labels))
    channel_requests = (
        parse_channel_measurements(document["channel_measurements"], labels)
        if "channel_measurements" in document
        else []
    )
    need_incident = loaded_noise or any(
        request["wave"] == "incident" for request in channel_requests
    )
    solve_options = None
    if "operating_point" in document:
        solve_options = dict(
            mixers=nonlinear_mixers,
            amplifiers=nonlinear_amplifiers,
            **operating_options(document["operating_point"], len(labels)),
        )
    result = library.conversion_network(
        spacing,
        devices,
        connections,
        reference_ohms=reference,
        loaded_noise=need_incident,
        additional_source_covariance=extra_c,
        additional_source_complementary=extra_p,
        output_offset=output_offsets if use_affine else None,
        operating_point=solve_options,
    )
    diagnostics = None
    if solve_options is not None:
        diagnostics = dict(
            method="damped_newton",
            iterations=result.iterations,
            backtracks=result.backtracks,
            scaled_residual=result.scaled_residual,
            relative_tolerance=solve_options["relative_tolerance"],
            absolute_tolerance_sqrt_w=solve_options["absolute_tolerance"],
        )
        result = result.waves
        update_converged_models(
            library,
            spacing,
            reference,
            devices,
            nonlinear_mixers,
            result,
            output_offsets,
            nonlinear_amplifiers,
        )
        use_affine = True
    operating_reports = check_operating_points(operating_points, result)
    channels = []
    for i, (name, port, index) in enumerate(labels):
        a, b = result.incident[i], result.outgoing[i]
        channels.append(
            {
                "device": name,
                "port": port,
                "bin": index,
                "frequency_hz": index * spacing,
                "connected": (name, port) in connected_ports,
                "incident": [a.real, a.imag],
                "outgoing": [b.real, b.imag],
                "outgoing_power_w": abs(b) ** 2,
            }
        )
    output = {
        "format": "rfmodel.conversion-network-result",
        "version": 1,
        "spacing_hz": spacing,
        "reference_ohms": reference,
        "wave_definition": "power",
        "channels": channels,
        "noise_covariance_w_per_hz": _encode(result.noise_covariance),
        "noise_complementary_w_per_hz": _encode(result.noise_complementary),
        "relative_residual": result.relative_residual,
    }
    if use_affine:
        output["wave_relation"] = "affine"
        output["output_offset"] = [[v.real, v.imag] for v in output_offsets]
    if diagnostics is not None:
        output["wave_relation"] = "nonlinear_operating_point"
        output["operating_point"] = diagnostics
    if operating_reports:
        output["operating_point_checks"] = operating_reports
    if phase_noise_groups:
        output["phase_noise_groups"] = phase_noise_groups
    if phase_noise_sources:
        output["phase_noise_sources"] = phase_noise_sources
    if loaded_noise:
        for field in (
            "incident_noise_covariance",
            "incident_noise_complementary",
            "incident_outgoing_noise_covariance",
            "incident_outgoing_noise_complementary",
        ):
            output[field + "_w_per_hz"] = _encode(getattr(result, field))
        for i, channel in enumerate(channels):
            channel["incident_noise_w_per_hz"] = result.incident_noise_covariance[i][i].real
            channel["outgoing_noise_w_per_hz"] = result.noise_covariance[i][i].real
            channel["net_noise_into_device_w_per_hz"] = result.net_noise_into_device_w_per_hz[i]
    if channel_requests:
        output["channel_measurements"] = measure_conversion_channels(
            library, channel_requests, labels, result, spacing
        )
    if "noise_analyses" in document:
        analyses = document["noise_analyses"]
        if not isinstance(analyses, list) or not 1 <= len(analyses) <= 512:
            raise ValueError("Expected 1..512 noise analyses")
        results, used_names = [], set()
        indices = {key: i for i, key in enumerate(labels)}

        def channel_index(channel):
            if (
                not isinstance(channel, list)
                or len(channel) != 3
                or not isinstance(channel[0], str)
                or type(channel[1]) is not int
                or type(channel[2]) is not int
                or tuple(channel) not in indices
            ):
                raise ValueError("Unknown noise analysis channel")
            return indices[tuple(channel)]

        for analysis in analyses:
            _object(
                analysis,
                ("name", "reference_channels", "thermal_channels", "output_channel"),
                ("reference_temperature_k",),
            )
            name = analysis["name"]
            if not isinstance(name, str) or not name or name in used_names:
                raise ValueError("Noise analysis names must be unique nonempty strings")
            used_names.add(name)
            if not isinstance(analysis["reference_channels"], list) or not isinstance(
                analysis["thermal_channels"], list
            ):
                raise ValueError("Noise reference channels must be lists")
            temperature = _number(analysis.get("reference_temperature_k", 290.0))
            metric = library.conversion_noise_analysis(
                spacing,
                devices,
                connections,
                reference_ohms=reference,
                reference_channels=[channel_index(c) for c in analysis["reference_channels"]],
                thermal_channels=[channel_index(c) for c in analysis["thermal_channels"]],
                output_channel=channel_index(analysis["output_channel"]),
                reference_temperature_k=temperature,
            )
            results.append(
                dict(
                    analysis,
                    reference_temperature_k=temperature,
                    normalization="independent_phase_averaged_available_power",
                    output_load_noise="excluded",
                    **metric._asdict(),
                )
            )
        output["noise_analyses"] = results
    return output

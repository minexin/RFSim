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
from .conversion_file import conversion_matrices
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
    _object(device, ("id", "bins", "model", "noise"), ("source_noise",))
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
        ("reference_ohms", "connections", "noise_analyses", "loaded_noise", "channel_measurements"),
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
        if isinstance(model, dict) and model.get("type") == "linear":
            native = linear_device(library, entry, spacing, reference, base_directory)
        else:
            _object(entry, ("id", "channels", "model", "noise"), ("source_noise",))
            if not isinstance(entry["channels"], list) or not 1 <= len(entry["channels"]) <= 512:
                raise ValueError("Expected conversion channel list")
            channels = []
            for channel in entry["channels"]:
                _object(channel, ("port", "bin"))
                if type(channel["port"]) is not int or type(channel["bin"]) is not int:
                    raise ValueError("Conversion indices must be integers")
                channels.append((channel["port"], channel["bin"]))
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
    channel_requests = (
        parse_channel_measurements(document["channel_measurements"], labels)
        if "channel_measurements" in document
        else []
    )
    need_incident = loaded_noise or any(
        request["wave"] == "incident" for request in channel_requests
    )
    result = library.conversion_network(
        spacing, devices, connections, reference_ohms=reference, loaded_noise=need_incident
    )
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

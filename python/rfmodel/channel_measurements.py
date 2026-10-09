"""Explicit port/direction channel measurements over conversion-network samples."""

from .model_file import _object, _number


def parse_channel_measurements(value, labels):
    if not isinstance(value, list) or not 1 <= len(value) <= 512:
        raise ValueError("Expected 1..512 channel measurements")
    ports = {(name, port) for name, port, index in labels}
    requests, names = [], set()
    for item in value:
        _object(item, ("name", "port", "center_hz", "bandwidth_hz", "desired_bins"), ("wave",))
        name = item["name"]
        if not isinstance(name, str) or not name or name in names:
            raise ValueError("Channel measurement names must be unique nonempty strings")
        names.add(name)
        port = item["port"]
        if (
            not isinstance(port, list)
            or len(port) != 2
            or not isinstance(port[0], str)
            or type(port[1]) is not int
            or tuple(port) not in ports
        ):
            raise ValueError("Unknown channel measurement port")
        bins = item["desired_bins"]
        if (
            not isinstance(bins, list)
            or any(type(b) is not int or b < 0 for b in bins)
            or len(set(bins)) != len(bins)
        ):
            raise ValueError("Desired bins must be unique nonnegative integers")
        available = {index for device, p, index in labels if (device, p) == tuple(port)}
        if not set(bins) <= available:
            raise ValueError("Desired bin is not represented at the measurement port")
        wave = item.get("wave", "outgoing")
        if wave not in ("incident", "outgoing"):
            raise ValueError("Channel measurement wave must be incident or outgoing")
        requests.append(
            dict(
                item,
                center_hz=_number(item["center_hz"]),
                bandwidth_hz=_number(item["bandwidth_hz"]),
                wave=wave,
            )
        )
    return requests


def measure_conversion_channels(library, requests, labels, result, spacing):
    measurements = []
    for request in requests:
        selected = sorted(
            (index, i)
            for i, (name, port, index) in enumerate(labels)
            if (name, port) == tuple(request["port"])
        )
        incident = request["wave"] == "incident"
        covariance = result.incident_noise_covariance if incident else result.noise_covariance
        waves = result.incident if incident else result.outgoing
        noise = [(index * spacing, covariance[i][i].real) for index, i in selected]
        desired = set(request["desired_bins"])
        lines = [(index * spacing, abs(waves[i]) ** 2) for index, i in selected if index in desired]
        metric = library.channel_noise(
            noise,
            center_hz=request["center_hz"],
            bandwidth_hz=request["bandwidth_hz"],
            desired_lines=lines,
        )
        measurements.append(dict(request, interpolation="linear_w_per_hz", **metric._asdict()))
    return measurements

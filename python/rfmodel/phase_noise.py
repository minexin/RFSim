"""Independent small-angle phase-noise sources for finite conversion grids."""

from .model_file import _object, _number


def apply_phase_noise_sources(library, specifications, devices, lookup, connected_ports):
    if not isinstance(specifications, list) or not 1 <= len(specifications) <= 512:
        raise ValueError("Expected 1..512 phase noise sources")
    names, carriers, descriptions = set(), set(), []
    for specification in specifications:
        _object(specification, ("name", "carrier_channel", "offsets"))
        name = specification["name"]
        if not isinstance(name, str) or not name or name in names:
            raise ValueError("Phase noise source names must be unique nonempty strings")
        names.add(name)
        channel = specification["carrier_channel"]
        if (
            not isinstance(channel, list)
            or len(channel) != 3
            or not isinstance(channel[0], str)
            or type(channel[1]) is not int
            or type(channel[2]) is not int
        ):
            raise ValueError("Invalid phase noise carrier channel")
        key = tuple(channel)
        if key not in lookup or key in carriers or key[:2] in connected_ports:
            raise ValueError("Phase noise carrier must be unique and externally driven")
        carriers.add(key)
        points = specification["offsets"]
        if not isinstance(points, list) or not 1 <= len(points) <= 255:
            raise ValueError("Expected 1..255 phase noise offset samples")
        offsets = []
        for point in points:
            _object(point, ("offset_bin", "ssb_dbc_per_hz"))
            if type(point["offset_bin"]) is not int:
                raise ValueError("Phase noise offset bin must be an integer")
            offsets.append((point["offset_bin"], _number(point["ssb_dbc_per_hz"])))
        device_index, local = lookup[key]
        device = devices[device_index]
        c, p = library.phase_noise_sidebands(
            device["channels"],
            carrier_channel=local,
            carrier_wave=device["source"][local],
            offsets=offsets,
        )
        count = len(device["channels"])
        for i in range(count):
            for j in range(count):
                device["source_covariance"][i][j] += c[i][j]
                device["source_complementary"][i][j] += p[i][j]
        descriptions.append(
            dict(
                specification,
                model="small_angle_paired_sidebands",
                correlation="independent_source",
                carrier_wave=[device["source"][local].real, device["source"][local].imag],
            )
        )
    return descriptions

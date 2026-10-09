"""Explicit RF/LO operating point and incremental signal/noise experiment."""

from .model_file import _object, _number, _complex, _encode
from .conversion_network_file import noise_pair
from .phase_noise import build_phase_noise_groups


def analyze_mixer_linearization(library, document, base_directory=None):
    _object(
        document,
        ("format", "version", "id", "spacing_hz", "channels", "operating_incident", "model"),
        (
            "reference_ohms",
            "perturbation_source",
            "perturbation_reflection",
            "source_noise",
            "phase_noise_groups",
            "loaded_noise",
        ),
    )
    if (
        document["format"] != "rfmodel.mixer-linearization"
        or type(document["version"]) is not int
        or document["version"] != 1
    ):
        raise ValueError("Unsupported mixer-linearization format/version")
    name = document["id"]
    if not isinstance(name, str) or not name:
        raise ValueError("Mixer id must be a nonempty string")
    entries = document["channels"]
    if not isinstance(entries, list) or not 1 <= len(entries) <= 512:
        raise ValueError("Expected 1..512 mixer channels")
    channels = []
    for channel in entries:
        _object(channel, ("port", "bin"))
        if type(channel["port"]) is not int or type(channel["bin"]) is not int:
            raise ValueError("Mixer port/bin must be integers")
        channels.append((channel["port"], channel["bin"]))
    n = len(channels)

    def vector(value):
        if not isinstance(value, list) or len(value) != n:
            raise ValueError("Mixer wave vector must cover every channel")
        return [_complex(v) for v in value]

    operating = vector(document["operating_incident"])
    model = document["model"]
    _object(model, ("lo_bin",), ("gain_db", "rf_port", "lo_port", "if_port"))
    if type(model["lo_bin"]) is not int:
        raise ValueError("LO bin must be an integer")
    ports = {
        field: model.get(field, default)
        for field, default in [("rf_port", 0), ("lo_port", 1), ("if_port", 2)]
    }
    if any(type(value) is not int for value in ports.values()):
        raise ValueError("Mixer port indices must be integers")
    spacing = _number(document["spacing_hz"])
    reference = _number(document.get("reference_ohms", 50.0))
    linearization = library.linearize_real_mixer(
        spacing,
        channels,
        operating,
        lo_bin=model["lo_bin"],
        gain_db=_number(model.get("gain_db", 0.0)),
        reference_ohms=reference,
        **ports,
    )
    source = vector(document.get("perturbation_source", [0] * n))
    reflection = vector(document.get("perturbation_reflection", [0] * n))
    c, p = noise_pair(document.get("source_noise", {"noiseless": True}), n)
    groups, extra_c, extra_p = [], None, None
    if "phase_noise_groups" in document:
        labels = [(name, port, index) for port, index in channels]
        lookup = {label: (0, i) for i, label in enumerate(labels)}
        groups, extra_c, extra_p = build_phase_noise_groups(
            library,
            document["phase_noise_groups"],
            [{"channels": channels, "source": operating}],
            labels,
            lookup,
            set(),
        )
    loaded = document.get("loaded_noise", False)
    if type(loaded) is not bool:
        raise ValueError("loaded_noise must be boolean")
    device = dict(
        channels=channels,
        direct=linearization.direct,
        conjugate=linearization.conjugate,
        source=source,
        reflection=reflection,
        source_covariance=c,
        source_complementary=p,
    )
    result = library.conversion_network(
        spacing,
        [device],
        reference_ohms=reference,
        loaded_noise=loaded,
        additional_source_covariance=extra_c,
        additional_source_complementary=extra_p,
    )
    rows = []

    def pair(value):
        return [value.real, value.imag]

    for i, (port, index) in enumerate(channels):
        rows.append(
            dict(
                port=port,
                bin=index,
                frequency_hz=index * spacing,
                operating_incident=pair(operating[i]),
                operating_outgoing=pair(linearization.operating_outgoing[i]),
                perturbation_incident=pair(result.incident[i]),
                perturbation_outgoing=pair(result.outgoing[i]),
            )
        )
    output = dict(
        format="rfmodel.mixer-linearization-result",
        version=1,
        id=name,
        analysis="incremental_about_supplied_operating_point",
        spacing_hz=spacing,
        reference_ohms=reference,
        channels=rows,
        direct=_encode(linearization.direct),
        conjugate=_encode(linearization.conjugate),
        noise_covariance_w_per_hz=_encode(result.noise_covariance),
        noise_complementary_w_per_hz=_encode(result.noise_complementary),
        relative_residual=result.relative_residual,
    )
    if groups:
        output["phase_noise_groups"] = groups
    if loaded:
        for field in (
            "incident_noise_covariance",
            "incident_noise_complementary",
            "incident_outgoing_noise_covariance",
            "incident_outgoing_noise_complementary",
        ):
            output[field + "_w_per_hz"] = _encode(getattr(result, field))
        output["net_noise_into_device_w_per_hz"] = list(result.net_noise_into_device_w_per_hz)
    return output

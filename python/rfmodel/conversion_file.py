"""Finite-channel fixed-pump conversion, reflection feedback, and C/P noise."""

from .model_file import _object, _number, _complex, _matrix, _encode


def analyze_conversion(library, document, base_directory=None):
    _object(
        document,
        ("format", "version", "spacing_hz", "channels", "model"),
        ("reference_ohms", "source", "reflection", "source_noise", "intrinsic_noise"),
    )
    if (
        document["format"] != "rfmodel.frequency-conversion"
        or type(document["version"]) is not int
        or document["version"] != 1
    ):
        raise ValueError("Unsupported frequency-conversion format/version")
    spacing = _number(document["spacing_hz"])
    reference = _number(document.get("reference_ohms", 50.0))
    entries = document["channels"]
    if not isinstance(entries, list) or not 1 <= len(entries) <= 512:
        raise ValueError("Conversion requires 1..512 channels")
    channels = []
    for entry in entries:
        _object(entry, ("port", "bin"))
        if type(entry["port"]) is not int or type(entry["bin"]) is not int:
            raise ValueError("Conversion channel indices must be integers")
        channels.append((entry["port"], entry["bin"]))
    model = document["model"]
    if not isinstance(model, dict):
        raise ValueError("Conversion model must be an object")
    kind = model.get("type")
    if kind == "matrix":
        _object(model, ("type", "direct"), ("conjugate",))
        direct = _matrix(model["direct"])
        conjugate = _matrix(model["conjugate"]) if "conjugate" in model else None
    elif kind == "ideal_real_mixer":
        _object(model, ("type", "lo_bin"), ("gain_db", "phase_radians", "rf_port", "if_port"))
        direct, conjugate = library.ideal_mixer_conversion(
            spacing,
            channels,
            lo_bin=model["lo_bin"],
            gain_db=_number(model.get("gain_db", 0.0)),
            phase_radians=_number(model.get("phase_radians", 0.0)),
            rf_port=model.get("rf_port", 0),
            if_port=model.get("if_port", 1),
            reference_ohms=reference,
        )
    else:
        raise ValueError("Unknown conversion model type")
    options = {}
    for field in ("source", "reflection"):
        if field in document:
            value = document[field]
            if not isinstance(value, list) or len(value) != len(channels):
                raise ValueError("Conversion boundary must cover every channel")
            options[field] = [_complex(v) for v in value]
    for field, prefix in (("source_noise", "source"), ("intrinsic_noise", "intrinsic")):
        if field in document:
            noise = document[field]
            _object(noise, ("covariance",), ("complementary",))
            options[prefix + "_covariance"] = _matrix(noise["covariance"])
            if "complementary" in noise:
                options[prefix + "_complementary"] = _matrix(noise["complementary"])
    result = library.frequency_conversion(
        spacing, channels, direct, conjugate, reference_ohms=reference, **options
    )
    points = []
    for index, (port, bin_index) in enumerate(channels):
        a, b = result.incident[index], result.outgoing[index]
        points.append(
            {
                "port": port,
                "bin": bin_index,
                "frequency_hz": bin_index * spacing,
                "incident": [a.real, a.imag],
                "outgoing": [b.real, b.imag],
                "outgoing_power_w": abs(b) ** 2,
            }
        )
    return {
        "format": "rfmodel.frequency-conversion-result",
        "version": 1,
        "spacing_hz": spacing,
        "reference_ohms": reference,
        "wave_definition": "power",
        "channels": points,
        "noise_covariance_w_per_hz": _encode(result.noise_covariance),
        "noise_complementary_w_per_hz": _encode(result.noise_complementary),
        "relative_residual": result.relative_residual,
    }

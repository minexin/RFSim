"""Absolute-wave offsets and supplied mixer operating-point consistency."""

from .model_file import _object, _number, _complex


def offset_vector(value, count):
    if not isinstance(value, list) or len(value) != count:
        raise ValueError("Output offset must cover every local channel")
    return [_complex(v) for v in value]


def linearized_mixer(library, spacing, channels, model, reference):
    _object(
        model,
        ("type", "lo_bin", "operating_incident"),
        ("gain_db", "rf_port", "lo_port", "if_port"),
    )
    if type(model["lo_bin"]) is not int:
        raise ValueError("Mixer LO bin must be an integer")
    ports = {
        field: model.get(field, value)
        for field, value in [("rf_port", 0), ("lo_port", 1), ("if_port", 2)]
    }
    if any(type(value) is not int for value in ports.values()):
        raise ValueError("Mixer port indices must be integers")
    operating = offset_vector(model["operating_incident"], len(channels))
    result = library.linearize_real_mixer(
        spacing,
        channels,
        operating,
        lo_bin=model["lo_bin"],
        gain_db=_number(model.get("gain_db", 0.0)),
        reference_ohms=reference,
        **ports,
    )
    return result, operating


def check_operating_points(checks, result):
    reports = []
    relative, absolute = 1e-9, 1e-12
    for name, offset, expected in checks:
        maximum, maximum_error = 0.0, 0.0
        for local, wave in enumerate(expected):
            actual = result.incident[offset + local]
            error = abs(actual - wave)
            component = max(abs(actual.real), abs(actual.imag), abs(wave.real), abs(wave.imag))
            magnitude = max(abs(actual / component), abs(wave / component)) if component else 0.0
            scale = absolute + (relative * component) * magnitude
            maximum = max(maximum, error / scale)
            maximum_error = max(maximum_error, error)
            if error > scale:
                raise ValueError(
                    f"Mixer {name!r} operating point inconsistent at local channel {local}: "
                    f"expected {wave!r}, solved {actual!r}"
                )
        reports.append(
            dict(
                device=name,
                passed=True,
                relative_tolerance=relative,
                absolute_tolerance_sqrt_w=absolute,
                max_scaled_error=maximum,
                max_error_sqrt_w=maximum_error,
            )
        )
    return reports

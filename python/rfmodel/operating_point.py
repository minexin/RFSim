"""Strict JSON configuration and converged-point model assembly."""

from .model_file import _object, _number, _complex


def bilinear_spec(model):
    _object(model, ("type", "lo_reference_amplitude"), ("gain_db", "rf_port", "lo_port", "if_port"))
    result = {
        "lo_reference_amplitude": _number(model["lo_reference_amplitude"]),
        "gain_db": _number(model.get("gain_db", 0.0)),
    }
    for name, default in [("rf_port", 0), ("lo_port", 1), ("if_port", 2)]:
        value = model.get(name, default)
        if type(value) is not int:
            raise ValueError("Mixer port must be an integer")
        result[name] = value
    return result


def amplifier_spec(model):
    _object(
        model,
        ("type", "power_gain_db", "output_p1db_dbm", "output_saturation_dbm"),
        ("input_port", "output_port", "include_output_drive"),
    )
    result = {
        field: _number(model[field])
        for field in ("power_gain_db", "output_p1db_dbm", "output_saturation_dbm")
    }
    for field, default in [("input_port", 0), ("output_port", 1)]:
        value = model.get(field, default)
        if type(value) is not int:
            raise ValueError("Amplifier port must be an integer")
        result[field] = value
    driven = model.get("include_output_drive", False)
    if type(driven) is not bool:
        raise ValueError("include_output_drive must be boolean")
    result["include_output_drive"] = driven
    return result


def polynomial_spec(model):
    _object(model, ("type", "voltage_coefficients"), ("input_port", "output_port"))
    coefficients = model["voltage_coefficients"]
    if not isinstance(coefficients, list) or not 1 <= len(coefficients) <= 12:
        raise ValueError("Polynomial requires 1..12 real voltage coefficients")
    result = {"voltage_coefficients": [_number(value) for value in coefficients]}
    for name, default in [("input_port", 0), ("output_port", 1)]:
        value = model.get(name, default)
        if type(value) is not int:
            raise ValueError("Polynomial port must be an integer")
        result[name] = value
    return result


def operating_options(value, total):
    _object(
        value,
        (),
        (
            "initial_incident",
            "max_iterations",
            "max_backtracks",
            "relative_tolerance",
            "absolute_tolerance",
        ),
    )
    options = {}
    for name, default in [("max_iterations", 50), ("max_backtracks", 24)]:
        item = value.get(name, default)
        if type(item) is not int:
            raise ValueError("Iteration limits must be integers")
        options[name] = item
    for name, default in [("relative_tolerance", 1e-9), ("absolute_tolerance", 1e-12)]:
        options[name] = _number(value.get(name, default))
    if "initial_incident" in value:
        initial = value["initial_incident"]
        if not isinstance(initial, list) or len(initial) != total:
            raise ValueError("Initial incident must cover every global channel")
        options["initial_incident"] = [_complex(v) for v in initial]
    return options


def update_converged_models(
    library, spacing, reference, devices, specs, waves, offsets, amplifiers=(), polynomials=()
):
    starts, total = [], 0
    for device in devices:
        starts.append(total)
        total += len(device["channels"])
    models = (
        [(s, library.linearize_bilinear_mixer) for s in specs]
        + [(s, library.linearize_saturating_amplifier) for s in amplifiers]
        + [(s, library.linearize_polynomial_amplifier) for s in polynomials]
    )
    for spec, linearize in models:
        parameters = dict(spec)
        index = parameters.pop("device")
        device, start = devices[index], starts[index]
        count = len(device["channels"])
        point = linearize(
            spacing,
            device["channels"],
            waves.incident[start : start + count],
            reference_ohms=reference,
            **parameters,
        )
        device["direct"], device["conjugate"] = point.direct, point.conjugate
        for local, emission in enumerate(point.output_offset):
            offsets[start + local] += emission

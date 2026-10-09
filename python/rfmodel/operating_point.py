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


def update_converged_mixers(library, spacing, reference, devices, specs, waves, offsets):
    starts, total = [], 0
    for device in devices:
        starts.append(total)
        total += len(device["channels"])
    for spec in specs:
        parameters = dict(spec)
        index = parameters.pop("device")
        device, start = devices[index], starts[index]
        count = len(device["channels"])
        point = library.linearize_bilinear_mixer(
            spacing,
            device["channels"],
            waves.incident[start : start + count],
            reference_ohms=reference,
            **parameters,
        )
        device["direct"], device["conjugate"] = point.direct, point.conjugate
        for local, emission in enumerate(point.output_offset):
            offsets[start + local] += emission

"""Versioned RF amplifier component analysis, without an implicit family sum."""
from .model_file import _object, _number, _matrix, analyze
from .spectrum_file import _decode, _encode


def _encode_terms(terms, spacing):
    return [dict(_encode({term.bin: term.amplitude}, spacing)[0],
                 order=term.order, contributors=list(term.contributors)) for term in terms]


def _propagate(library, terms, spacing, reference, network, base_directory):
    bins = sorted({term.bin for term in terms})
    document = dict(network, format="rfmodel.linear-network", version=1, reference_ohms=reference,
                    frequencies_hz=[index * spacing for index in bins] or [0.])
    samples = analyze(library, document, base_directory=base_directory)["samples"]
    transmitted = {}
    for index, sample in zip(bins or [0], samples):
        selected = [term for term in terms if term.bin == index]
        with library.network(reference) as reduced:
            reduced.add(_matrix(sample["s"]))
            for term in reduced.transmit_terms(spacing, selected, [0, 1]):
                transmitted[(term.order, term.bin, term.contributors)] = term
    return tuple(transmitted[(term.order, term.bin, term.contributors)] for term in terms)


def analyze_amplifier(library, document, *, base_directory=None):
    _object(document, ("format", "version", "spacing_hz", "input", "parameters"),
            ("reference_ohms", "include_terms", "post_stages"))
    if (document["format"] != "rfmodel.amplifier-components" or
            type(document["version"]) is not int or document["version"] != 1):
        raise ValueError("Unsupported amplifier component format/version")
    spacing = _number(document["spacing_hz"])
    reference = _number(document.get("reference_ohms", 50.))
    if spacing <= 0 or reference <= 0:
        raise ValueError("Spacing and reference impedance must be positive")
    keys = ("power_gain_db", "output_p1db_dbm", "output_saturation_dbm", "input_ip2_dbm", "input_ip3_dbm")
    _object(document["parameters"], keys)
    parameters = {key: _number(document["parameters"][key]) for key in keys}
    include_terms = document.get("include_terms", False)
    if type(include_terms) is not bool:
        raise ValueError("include_terms must be a boolean")
    stages = document.get("post_stages", [])
    if not isinstance(stages, list) or len(stages) > 128:
        raise ValueError("post_stages must contain at most 128 linear stages")
    if stages and not include_terms:
        raise ValueError("post_stages requires include_terms=true")
    identifiers = set()
    for stage in stages:
        _object(stage, ("id", "type", "network"))
        identifier = stage["id"]
        if not isinstance(identifier, str) or not identifier or identifier in identifiers:
            raise ValueError("Post-stage IDs must be unique nonempty strings")
        identifiers.add(identifier)
        if stage["type"] != "linear_network":
            raise ValueError("Only linear post-stages preserve this mixing identity contract")
        network = stage["network"]
        _object(network, ("devices", "external_ports"), ("connections", "terminations"))
        if not isinstance(network["external_ports"], list) or len(network["external_ports"]) != 2:
            raise ValueError("Post-stage requires two external ports")
        if not isinstance(network["devices"], list):
            raise ValueError("Post-stage devices must be an array")
        for device in network["devices"]:
            _object(device, ("id",), ("s", "model"))
    amplitudes = _decode(document["input"], spacing)
    response = library.multitone_amplifier(spacing, amplitudes,
                                           reference_ohms=reference, **parameters)
    result = {"format": "rfmodel.amplifier-components-result", "version": 1,
            "spacing_hz": spacing, "reference_ohms": reference,
            "total_input_power_w": response.total_input_power_w,
            "limited_input_power_w": response.limited_input_power_w,
            "direct": _encode(response.direct, spacing),
            "second_order": _encode(response.second_order, spacing),
            "third_order": _encode(response.third_order, spacing)}
    if include_terms:
        traced = library.multitone_amplifier_terms(spacing, amplitudes, reference_ohms=reference, **parameters)
        result["terms"] = _encode_terms(traced.terms, spacing)
        if "post_stages" in document:
            terms, path = traced.terms, []
            result["post_stages"] = []
            for stage in stages:
                try:
                    terms = _propagate(library, terms, spacing, reference, stage["network"], base_directory)
                except (ValueError, RuntimeError, OverflowError) as error:
                    raise ValueError("Post-stage '" + stage["id"] + "': " + str(error)) from error
                path.append(stage["id"])
                result["post_stages"].append({"id": stage["id"], "path": list(path),
                                               "terms": _encode_terms(terms, spacing)})
    return result

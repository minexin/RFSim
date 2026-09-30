"""Versioned RF amplifier component analysis, without an implicit family sum."""
from .model_file import _object, _number
from .spectrum_file import _decode, _encode


def analyze_amplifier(library, document, *, base_directory=None):
    _object(document, ("format", "version", "spacing_hz", "input", "parameters"), ("reference_ohms",))
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
    response = library.multitone_amplifier(spacing, _decode(document["input"], spacing),
                                           reference_ohms=reference, **parameters)
    return {"format": "rfmodel.amplifier-components-result", "version": 1,
            "spacing_hz": spacing, "reference_ohms": reference,
            "total_input_power_w": response.total_input_power_w,
            "limited_input_power_w": response.limited_input_power_w,
            "direct": _encode(response.direct, spacing),
            "second_order": _encode(response.second_order, spacing),
            "third_order": _encode(response.third_order, spacing)}

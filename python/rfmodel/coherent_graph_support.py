"""Shared label and array bounds for coherent graph execution."""


def _label(value):
    if (
        not isinstance(value, str)
        or not value
        or chr(0) in value
        or len(value.encode("utf-8")) > 1024
    ):
        raise ValueError("Expected a nonempty label of at most 1024 UTF-8 bytes")
    return value


def _array(value, maximum, *, nonempty=False):
    if not isinstance(value, list) or len(value) > maximum or (nonempty and not value):
        raise ValueError("Invalid graph array size")
    return value

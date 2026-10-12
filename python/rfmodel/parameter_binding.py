"""Shared numeric-leaf bindings and measurement paths for declarative RF tasks."""
import copy
import hashlib
import math
from .model_file import _number, _complex, referenced_touchstone_paths

_COMPONENTS = ('value', 'real', 'imag', 'magnitude', 'power', 'magnitude_db', 'phase_radians')


def _array(value, limit, label):
    if not isinstance(value, list) or not 1 <= len(value) <= limit:
        raise ValueError(f'{label} requires 1..{limit} entries')
    return value


def _name(value):
    if not isinstance(value, str) or not value or '\x00' in value:
        raise ValueError('Expected a nonempty name without NUL')
    return value


def _path(value):
    path = _array(value, 64, 'Path')
    for segment in path:
        if isinstance(segment, str):
            continue
        if type(segment) is int and segment >= 0:
            continue
        if isinstance(segment, dict) and len(segment) == 1:
            key, expected = next(iter(segment.items()))
            if isinstance(key, str) and isinstance(expected, str):
                continue
            if isinstance(key, str) and type(expected) in (int, float):
                _number(expected)
                continue
        raise ValueError('Path segments must be keys, nonnegative indices or one-field string/number selectors')
    return copy.deepcopy(path)


def _resolve(root, path):
    """Return a value and its canonical key/index path; require unique selectors."""
    value, canonical = root, []
    for segment in path:
        if isinstance(value, dict) and isinstance(segment, str) and segment in value:
            key = segment
        elif isinstance(value, list) and type(segment) is int and segment < len(value):
            key = segment
        elif isinstance(value, list) and isinstance(segment, dict):
            field, expected = next(iter(segment.items()))
            matches = [i for i, entry in enumerate(value)
                       if isinstance(entry, dict) and field in entry
                       and ((isinstance(expected, str) and isinstance(entry[field], str))
                            or (type(expected) in (int, float) and type(entry[field]) in (int, float)))
                       and entry[field] == expected]
            if len(matches) != 1:
                raise ValueError('Path selector must match exactly one entry: ' + repr(segment))
            key = matches[0]
        else:
            raise ValueError('Path does not resolve: ' + repr(path))
        value = value[key]
        canonical.append(key)
    return value, tuple(canonical)


def _assign(root, path, value):
    for key in path[:-1]:
        root = root[key]
    root[path[-1]] = value


def _bound_value(value, scale, offset, integral):
    if integral:
        if abs(scale) > 2**53-1 or abs(offset) > 2**53-1:
            raise ValueError('Integer binding coefficients exceed exact integer range')
        result = int(value)*int(scale)+int(offset)
        if abs(result) > 2**53-1:
            raise ValueError('Integer binding result exceeds exact integer range')
        return result
    return _number(value*scale+offset)


def _fingerprints(document, base_directory):
    result = []
    for path in sorted(referenced_touchstone_paths(document, base_directory), key=str):
        digest = hashlib.sha256()
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1024*1024), b''):
                digest.update(block)
        result.append(dict(path=str(path), sha256=digest.hexdigest()))
    return result


def _measurement(value, component):
    if component == 'value':
        return _number(value)
    value = _complex(value)
    if component == 'real':
        return value.real
    if component == 'imag':
        return value.imag
    if component == 'magnitude':
        return abs(value)
    if component == 'power':
        return abs(value)**2
    if component == 'magnitude_db':
        return 20*math.log10(abs(value))
    return math.atan2(value.imag, value.real)

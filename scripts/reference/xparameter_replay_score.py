"""Developer-only exact format-evidence scorer. Never parses vendor input files."""
import hashlib
import json
import math
import re
from pathlib import Path

BASELINE_SHA256 = '432022e69d4cb1010b27d73b18adc03488d106989f69900ccc5592421963ec59'
FILE_IDS = ('ZFL_11AD_PHDModel.mdf', 'X-param_BalAmp_ZFL11AD.mdf',
            'Multitone_Volterra_X-device.xnp', 'AMP_Xparams.mdf')
RELATIVE_DIRECTORY = Path('Examples/RF Architecture Design/RF Design Kit/X Parameters')
MAX_SUMMARY_BYTES = 256 * 1024
NATIVE_KEYS = frozenset(('schema_version', 'reader_version', 'simulation_executed',
    'numerical_model_compatibility', 'all_data_group_shapes_checked', 'block_counts',
    'coefficient_rows_per_block', 'coefficient_columns_per_row', 'first_block_column_families',
    'AN_raw_ranges', 'first_row_small_sample', 'metadata_blocks',
    'coefficient_block_outer_variables', 'legacy_constants'))

class ReplayError(ValueError):
    pass

def sha256(data):
    return hashlib.sha256(data).hexdigest()

def strict_json(data, limit=MAX_SUMMARY_BYTES):
    if not isinstance(data, bytes) or len(data) > limit:
        raise ReplayError('JSON byte limit or input type')
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ReplayError('duplicate JSON key')
            result[key] = value
        return result
    def constant(_):
        raise ReplayError('nonfinite JSON value')
    try:
        value = json.loads(data.decode('utf-8', errors='strict'),
                           object_pairs_hook=pairs, parse_constant=constant)
    except (UnicodeError, ValueError, RecursionError) as exc:
        raise ReplayError('invalid JSON') from exc
    def finite(item):
        if isinstance(item, float) and not math.isfinite(item):
            raise ReplayError('nonfinite JSON number')
        if isinstance(item, str):
            try:
                item.encode('utf-8', errors='strict')
            except UnicodeError as exc:
                raise ReplayError('invalid Unicode string') from exc
        if isinstance(item, dict):
            for key, val in item.items():
                finite(key)
                finite(val)
        elif isinstance(item, list):
            for val in item:
                finite(val)
    try:
        finite(value)
    except RecursionError as exc:
        raise ReplayError('JSON nesting limit') from exc
    return value

def exact(actual, expected, path='summary'):
    """Structural equality: bool never numeric; explicit record type carries real/int.

    JSON has no reliable real-number lexical type after a C++ round trip. For
    expected float values, either finite JSON numeric representation is allowed.
    Expected integer structural fields and integer typed records remain integers.
    """
    if type(expected) is float:
        good = type(actual) in (int, float) and actual == expected and (
            type(actual) is int or math.isfinite(actual))
        if good and expected == 0.0:
            good = math.copysign(1.0, actual) == math.copysign(1.0, expected)
    elif type(actual) is not type(expected):
        good = False
    elif isinstance(expected, dict):
        good = actual.keys() == expected.keys()
        if good:
            for key in expected:
                exact(actual[key], expected[key], path + '.' + key)
    elif isinstance(expected, list):
        good = len(actual) == len(expected)
        if good:
            for i, (left, right) in enumerate(zip(actual, expected)):
                exact(left, right, path + '[' + str(i) + ']')
    else:
        good = actual == expected
    if not good:
        raise ReplayError('mismatch at ' + path)

# This deliberately tiny grammar consumes ONLY the frozen inspector's VAR/#
# declarations, never a candidate .mdf/.xnp file. Unknown syntax is an error.
_DECIMAL = r'[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?'
_DECLARATION = re.compile(r'(VAR|#) ([A-Za-z_][A-Za-z_0-9]*)\((real|int|complex|string)\) = (.+)')

def declaration(text, marker):
    match = _DECLARATION.fullmatch(text)
    if not match or match[1] != marker:
        raise ReplayError('unsupported baseline declaration')
    name, kind, token = match[2], match[3], match[4]
    if kind == 'real' and re.fullmatch(_DECIMAL, token):
        value = float(token)
    elif kind == 'int' and re.fullmatch(r'[+-]?[0-9]+', token):
        value = int(token)
        kind = 'integer'
    elif kind == 'complex' and re.fullmatch(_DECIMAL + ' ' + _DECIMAL, token):
        value = [float(x) for x in token.split(' ')]
    else:
        # Current immutable evidence contains real declarations only. Do not
        # invent a string decoding grammar or silently fill unsupported fields.
        raise ReplayError('unsupported baseline declaration value')
    if any(not math.isfinite(x) for x in (value if isinstance(value, list) else [value])):
        raise ReplayError('nonfinite baseline declaration')
    return {'name': name, 'type': kind, 'value': value}

_METADATA_TYPES = {'XParamAttributes': {'Index': 'integer', 'Version': 'real',
    'NumPorts': 'integer', 'NumFundFreqs': 'integer'}, 'XParamPortData': {
    'PortNumber': 'integer', 'RefZ0': 'complex', 'PortName': 'string'}}

def project_file(item):
    """Project immutable prior evidence, not observed native output."""
    file_id = item['path'].replace('\\', '/').rsplit('/', 1)[-1]
    if file_id not in FILE_IDS:
        raise ReplayError('unknown baseline file')
    metadata = {}
    for name, rows in item['metadata_blocks'].items():
        if name not in _METADATA_TYPES:
            raise ReplayError('unknown baseline metadata block')
        projected = []
        for row in rows:
            if set(row) != set(_METADATA_TYPES[name]):
                raise ReplayError('unknown/missing baseline metadata field')
            projected.append([{'name': key, 'type': _METADATA_TYPES[name][key], 'value': val}
                              for key, val in row.items()])
        metadata[name] = projected
    return {'schema_version': 1, 'file_id': file_id, 'bytes': item['bytes'],
        'sha256': item['sha256'], 'reader_version': '2.0' if metadata else 'legacy',
        'simulation_executed': False, 'numerical_model_compatibility': False,
        'all_data_group_shapes_checked': True,
        **{key: item[key] for key in ('block_counts', 'coefficient_rows_per_block',
            'coefficient_columns_per_row', 'first_block_column_families', 'AN_raw_ranges')},
        'metadata_blocks': metadata,
        'coefficient_block_outer_variables': [[declaration(x, 'VAR') for x in block]
            for block in item['coefficient_block_outer_variables']],
        'legacy_constants': [[declaration(x, '#') for x in block]
            for block in item['legacy_constants']],
        'first_row_small_sample': [{'name': key,
            'type': 'complex' if isinstance(val, list) else 'real', 'value': val}
            for key, val in item['first_row_small_sample'].items()]}

def load_baseline(path):
    data = Path(path).read_bytes()
    if sha256(data) != BASELINE_SHA256:
        raise ReplayError('baseline SHA256 mismatch')
    baseline = strict_json(data)
    expected = [project_file(item) for item in baseline['files']]
    if tuple(x['file_id'] for x in expected) != FILE_IDS:
        raise ReplayError('baseline selected file order mismatch')
    return baseline, expected

def score_selected(observed, expected):
    """All-or-nothing comparison. Public collection loads pinned baseline first."""
    if type(observed) is not list or len(observed) != 4:
        raise ReplayError('exactly four file summaries required')
    if tuple(x.get('file_id') for x in observed if type(x) is dict) != FILE_IDS:
        raise ReplayError('selected file identity/order mismatch')
    if tuple(x.get('file_id') for x in expected if type(x) is dict) != FILE_IDS:
        raise ReplayError('expected file identity/order mismatch')
    for actual, wanted in zip(observed, expected):
        # Identity gates precede all shape or value comparisons.
        exact(actual.get('bytes'), wanted['bytes'], wanted['file_id'] + '.bytes')
        exact(actual.get('sha256'), wanted['sha256'], wanted['file_id'] + '.sha256')
    for actual, wanted in zip(observed, expected):
        exact(actual, wanted, wanted['file_id'])
    hashes = [x['sha256'] for x in observed]
    duplicates = [[x['file_id'] for x in observed if x['sha256'] == digest]
                  for digest in dict.fromkeys(hashes) if hashes.count(digest) > 1]
    return {'selected_files': 4, 'unique_content_hashes': len(set(hashes)),
            'duplicate_content_groups': duplicates, 'independent_simulations': 0}

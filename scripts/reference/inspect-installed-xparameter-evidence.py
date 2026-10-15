"""Read selected installed vendor examples as format evidence, not a model importer.

No vendor file is copied into the repository. Only schema counts, hashes and a
small coefficient/axis sample are emitted. No interpolation or simulation runs.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import shlex
import sqlite3
import zlib


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def blocks(text):
    """Bounded syntax reader for the selected examples; preserve line grouping."""
    result, variables = [], []
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        line = lines[index].strip()
        index += 1
        if line.startswith('VAR '):
            variables.append(line)
        if not line.startswith('BEGIN '):
            continue
        block = {'name': line.split()[1], 'variables': variables, 'constants': [], 'headers': [], 'rows': []}
        variables = []
        data = []
        while index < len(lines):
            line = lines[index].strip()
            index += 1
            if line == 'END':
                break
            if not line or line.startswith(('!', 'REM')):
                continue
            if line.startswith('#'):
                block['constants'].append(line)
            elif line.startswith('%'):
                require(not data, 'Unexpected header inside data groups')
                declarations = re.findall(r'(\w+)\((\w+)\)', line[1:])
                require(declarations, 'Unrecognized header')
                block['headers'].append(declarations)
            else:
                data.append(shlex.split(line))
        else:
            raise ValueError('Unterminated data block')
        headers = block['headers']
        require(headers and len(data) % len(headers) == 0, 'Incomplete multiline data group')
        for offset in range(0, len(data), len(headers)):
            row = {}
            for header, values in zip(headers, data[offset:offset + len(headers)]):
                require(len(values) == sum(2 if kind == 'complex' else 1 for _, kind in header),
                        'Header/data line width mismatch')
                cursor = 0
                for name, kind in header:
                    require(name not in row, 'Duplicate variable')
                    width = 2 if kind == 'complex' else 1
                    if kind == 'string':
                        value = values[cursor]
                    else:
                        require(kind in ('int', 'real', 'complex'), 'Unsupported selected-file type')
                        numbers = [float(v) for v in values[cursor:cursor + width]]
                        require(all(math.isfinite(v) for v in numbers), 'Nonfinite selected-file value')
                        value = numbers if width == 2 else numbers[0]
                        if kind == 'int':
                            require(value == int(value), 'Noninteger int field')
                            value = int(value)
                    row[name] = value
                    cursor += width
            block['rows'].append(row)
        result.append(block)
    require(result, 'No blocks')
    return result


def inspect(path):
    original = path.read_bytes()
    text = original.decode('utf-8-sig')
    parsed = blocks(text)
    data = [b for b in parsed if b['name'] in ('PHDModelData', 'XParamData')]
    require(data, 'No X/PHD coefficient data')
    families = Counter(name.split('_', 1)[0] for name in data[0]['rows'][0])
    axes = {}
    for block in data:
        for name in block['rows'][0]:
            if name.startswith('AN_'):
                values = [r[name] for r in block['rows']]
                axes.setdefault(name, []).extend(values)
    first = data[0]['rows'][0]
    sample_names = [name for name in first if name.startswith(('FB_2_', 'S_2_', 'T_2_', 'FI_'))][:5]
    return {'path': path.as_posix(), 'bytes': len(original), 'sha256': digest(original),
            'block_counts': dict(Counter(b['name'] for b in parsed)),
            'metadata_blocks': {b['name']: b['rows'] for b in parsed
                                if b['name'] in ('XParamAttributes', 'XParamPortData')},
            'coefficient_block_outer_variables': [b['variables'] for b in data],
            'legacy_constants': [b['constants'] for b in data if b['constants']],
            'coefficient_rows_per_block': [len(b['rows']) for b in data],
            'coefficient_columns_per_row': [len(b['rows'][0]) for b in data],
            'first_block_column_families': dict(families),
            'AN_raw_ranges': {name: {'min': min(values), 'max': max(values),
                                    'first': values[0]} for name, values in axes.items()},
            'first_row_small_sample': {name: first[name] for name in sample_names},
            'leading_comments': [line for line in text.splitlines()[:20] if line.startswith('!')],
            'all_data_group_shapes_checked': True,
            'reference_plane_identification': 'Not established by inspected schema or port names',
            'numerical_model_compatibility': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--installation-root', type=Path, default=Path('C:/Program Files/Keysight/SystemVue2023'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), 'Refuse to overwrite evidence')
    examples = args.installation_root / 'Examples/RF Architecture Design/RF Design Kit/X Parameters'
    filenames = ('ZFL_11AD_PHDModel.mdf', 'X-param_BalAmp_ZFL11AD.mdf',
                 'Multitone_Volterra_X-device.xnp', 'AMP_Xparams.mdf')
    records = [inspect(examples / name) for name in filenames]
    help_file = args.installation_root / 'Help/systemvue.qch'
    help_records = []
    connection = sqlite3.connect(help_file.resolve().as_uri() + '?mode=ro', uri=True)
    try:
        for name in ('rfdesign/XPARAMS.html', 'users/Theory_of_Operation.html',
                     'docimages/portdata_eqn.gif', 'docimages/xnp_eqn1.gif'):
            index, = connection.execute('SELECT FileId FROM FileNameTable WHERE Name=?', (name,)).fetchone()
            data, = connection.execute('SELECT Data FROM FileDataTable WHERE Id=?', (index,)).fetchone()
            expanded = zlib.decompress(data[4:])
            help_records.append({'qch_resource': name, 'bytes': len(expanded), 'sha256': digest(expanded)})
    finally:
        connection.close()
    result = {'schema_version': 1, 'classification': 'installed_vendor_format_evidence_no_simulation',
              'created_utc': datetime.now(timezone.utc).isoformat(),
              'systemvue_version': '2023.0.0.11903', 'files': records,
              'README': {'sha256': digest((examples / 'README.txt').read_bytes()),
                         'content': (examples / 'README.txt').read_text(encoding='utf-8-sig')},
              'help': {'path': help_file.as_posix(), 'sha256': digest(help_file.read_bytes()),
                       'resources': help_records},
              'inspector_source_sha256_lf': digest(Path(__file__).read_bytes().replace(b'\r\n', b'\n')),
              'limits': ['Selected installed examples, not a complete vendor parser or compatibility claim.',
                         'No vendor model was imported, evaluated, fitted or simulated.',
                         'Legacy pre-2.0 normalization and physical reference planes remain unresolved.',
                         'Header comments and README statements are evidence, not authoritative numerical axis units.']}
    with args.output.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print(json.dumps({Path(r['path']).name: {'blocks': r['block_counts'],
                     'rows': r['coefficient_rows_per_block'], 'columns': r['coefficient_columns_per_row'],
                     'families': r['first_block_column_families'], 'AN': r['AN_raw_ranges']} for r in records}, indent=2))


if __name__ == '__main__':
    main()

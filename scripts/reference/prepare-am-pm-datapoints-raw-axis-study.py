"""Prepare independent DataPoints inputs by exact XML byte edits; no COM calls."""

import argparse
import copy
from decimal import Decimal, InvalidOperation, localcontext
import hashlib
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET
import xml.parsers.expat


SOURCE_PATH = 'build-reference/ampm-parameter-study-20261013/RFModel_AMPM_A01.wsv'
SOURCE_SHA256 = 'ead90a212c786eff0cfa782dab47dbfe14c421b3f43b32c573bb990cb8348877'
PLAN_SHA256 = 'd6d2624f410b35a458988e82bb619b1548f67fd3612adc555b569842d9b125f7'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(payload):
    return hashlib.sha256(payload).hexdigest()


def decimal(value):
    require(type(value) in (int, float, str), 'Finite decimal parameter required')
    try:
        result = Decimal(str(value))
    except InvalidOperation as error:
        raise ValueError('Finite decimal parameter required') from error
    require(result.is_finite(), 'Finite decimal parameter required')
    return result


def expression(value):
    value = value if isinstance(value, Decimal) else decimal(value)
    require(value.is_finite(), 'Finite decimal expression required')
    text = format(value, 'f')
    if '.' in text:
        text = text.rstrip('0').rstrip('.')
    return '0' if value == 0 else text


def source_expression(target, gain):
    target, gain = decimal(target), decimal(gain)
    with localcontext() as context:
        context.prec = max(28, len(format(target, 'f')) + len(format(gain, 'f')) + 4)
        return expression(target - gain)


def planned_fields(profile):
    """Map the seventeen registered XML field paths to their expression text."""
    name = profile.get('name')
    require(isinstance(name, str) and re.fullmatch(r'[a-z0-9][a-z0-9_-]*', name), 'Unsafe profile name')
    require(name.upper() not in {'CON', 'PRN', 'AUX', 'NUL'}
            and not re.fullmatch(r'(?:COM|LPT)[1-9]', name.upper()), 'Reserved profile name')
    powers, phases = profile['power_dbm'], profile['phase_deg']
    require(isinstance(powers, list) and isinstance(phases, list)
            and len(powers) == len(phases) and len(powers) >= 2, 'Invalid table shape')
    powers, phases = list(map(decimal, powers)), list(map(decimal, phases))
    require(all(a < b for a, b in zip(powers, powers[1:])), 'Power table must increase')
    start, stop, step = [decimal(profile[key]) for key in
                         ('sweep_input_start_dbm', 'sweep_input_stop_dbm', 'sweep_step_db')]
    require(stop > start and step > 0, 'Invalid sweep interval')
    frequency = decimal(profile['frequency_hz'])
    require(frequency > 0, 'Invalid frequency')
    amp = 'DataPoints/Sch2/PartList/RFAmp/ParamSet/'
    source = 'DataPoints/Sch2/PartList/Source/ParamSet/'
    sweep = 'DataPoints/System2/Path1/'
    values = {amp + name + '/DataEntry': expression(value) for name, value in (
        ('AMtoPM_Mode', 2), ('G', profile['gain_db']), ('OP1dB', profile['output_p1db_dbm']),
        ('OPSAT', profile['output_saturation_dbm']), ('OIP2', profile['output_ip2_dbm']),
        ('OIP3', profile['output_ip3_dbm']))}
    for name, table in (('PwrAMtoPMpoints', powers), ('AMtoPMpoints', phases)):
        values[amp + name + '/DataEntry'] = '[' + ','.join(map(expression, table)) + ']'
    for name, value in (
        ('Pwr', source_expression(profile['ordinary_output_target_dbm'], profile['gain_db'])),
        ('Phase', profile['source_phase_deg']), ('Freq', frequency / Decimal('1000000'))):
        values[source + name + '/DataEntry'] = expression(value)
    for name, value in (('QCStartPwr', start), ('QCStopPwr', stop), ('QCStepPwr', step),
                        ('QCFreq', frequency / Decimal('1000000'))):
        values[sweep + name + '/DataEntry'] = expression(value)
    values[sweep + 'QCAutoLimits/Data/D'] = '0'
    values[sweep + 'QCSwpType/Data/D'] = '3'
    require(len(values) == 17, 'Unexpected preparation field count')
    return values


def element(root, path):
    parts = path.split('/')
    current = root
    while parts and parts[0] not in ('DataEntry', 'Data'):
        name = parts.pop(0)
        matches = [child for child in current.findall('Item') if child.get('XName') == name]
        require(len(matches) == 1, 'Missing or duplicate XML item: ' + name)
        current = matches[0]
    matches = current.findall('/'.join(parts))
    require(len(matches) == 1, 'Missing or duplicate XML field: ' + path)
    return matches[0]


def signature(node):
    return node.tag, dict(node.attrib), node.text, node.tail, tuple(signature(child) for child in node)


def prepare_document(raw, profile):
    """Return changed bytes and change ledger, auditing the entire inverse tree."""
    values = planned_fields(profile)
    original = ET.fromstring(raw)
    expected = copy.deepcopy(original)
    changes = []
    for path, after in values.items():
        target = element(expected, path)
        changes.append(dict(path=path, before=target.text, after=after))
        target.text = after
    spans, stack, opened = {}, [], {}
    parser = xml.parsers.expat.ParserCreate()

    def start(tag, attrs):
        stack.append(attrs.get('XName', tag) if stack else '')
        path = '/'.join(stack[1:])
        if path in values:
            require(path not in opened, 'Repeated XML byte edit target: ' + path)
            opened[path] = parser.CurrentByteIndex, tag

    def end(tag):
        path = '/'.join(stack[1:])
        if path in opened:
            begin, name = opened[path]
            opening_end = raw.index(b'>', begin) + 1
            empty = raw[begin:opening_end].rstrip().endswith(b'/>')
            end_offset = opening_end if empty else raw.index(b'>', parser.CurrentByteIndex) + 1
            require(path not in spans, 'Repeated XML byte edit target: ' + path)
            replacement = ('<' + name + '>' + values[path] + '</' + name + '>').encode('ascii')
            spans[path] = begin, end_offset, replacement
        stack.pop()

    parser.StartElementHandler, parser.EndElementHandler = start, end
    parser.Parse(raw, True)
    require(set(spans) == set(values), 'Not all XML byte spans found')
    result = raw
    for begin, end_offset, replacement in sorted(spans.values(), reverse=True):
        result = result[:begin] + replacement + result[end_offset:]
    require(signature(ET.fromstring(result)) == signature(expected), 'Byte edits differ from planned semantics')
    for change in changes:
        element(expected, change['path']).text = change['before']
    require(signature(expected) == signature(original), 'Inverse semantic audit failed')
    return result, changes


def prepare(root, plan_path, output_directory):
    root = Path(root).resolve()
    plan_path = Path(plan_path).resolve()
    output_directory = Path(output_directory).resolve()
    require(output_directory.is_relative_to(root) and output_directory != root,
            'Output directory must remain inside root')
    require(not output_directory.exists(), 'Preparation output already exists')
    plan_bytes = plan_path.read_bytes()
    require(digest(plan_bytes) == PLAN_SHA256, 'Registered plan hash changed')
    plan = json.loads(plan_bytes)
    require(plan['source_workspace_sha256'] == SOURCE_SHA256, 'Plan source hash changed')
    source = (root / SOURCE_PATH).resolve()
    require(source.is_relative_to(root), 'Source workspace escaped root')
    raw = source.read_bytes()
    require(digest(raw) == SOURCE_SHA256, 'Source workspace hash changed')
    profiles = plan['profiles']
    require(isinstance(profiles, list) and profiles, 'No planned profiles')
    require(len({profile['name'] for profile in profiles}) == len(profiles), 'Duplicate profile names')
    prepared = [(profile, *prepare_document(raw, profile)) for profile in profiles]
    # Validate every profile before creating any destination files.
    output_directory.mkdir(parents=True, exist_ok=False)
    manifest = dict(schema_version=1, plan_sha256=digest(plan_bytes), source_path=SOURCE_PATH,
                    source_sha256=SOURCE_SHA256, profiles=[])
    for profile, data, changes in prepared:
        workspace = output_directory / 'profiles' / profile['name'] / 'RFModel_AMPM_A01.wsv'
        workspace.parent.mkdir(parents=True, exist_ok=False)
        with workspace.open('xb') as stream:
            stream.write(data)
        manifest['profiles'].append(dict(name=profile['name'], workspace_path=workspace.relative_to(root).as_posix(),
                                        workspace_sha256=digest(data), changes=changes))
    with (output_directory / 'prepared-workspaces.json').open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(manifest, stream, indent=2, allow_nan=False)
        stream.write('\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('plan', type=Path)
    parser.add_argument('output_directory', type=Path)
    args = parser.parse_args()
    result = prepare(args.root, args.plan, args.output_directory)
    print('Prepared', len(result['profiles']), 'independent profiles with 17 audited XML byte edits each.')


if __name__ == '__main__':
    main()

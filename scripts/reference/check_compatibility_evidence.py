#!/usr/bin/env python3
"""Developer-only evidence metadata check; never runs or reclassifies vendor tests."""
import argparse
import copy
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def schema_check(value, schema, where='$'):
    """Validate the explicitly supported Draft 2020-12 keyword subset, fail closed."""
    supported = {'$schema', '$id', 'title', 'type', 'properties', 'required',
                 'additionalProperties', 'items', 'minItems', 'minLength',
                 'minimum', 'pattern', 'enum', 'const'}
    require(set(schema) <= supported, f'{where}: unsupported schema keyword')
    types = {'object': dict, 'array': list, 'string': str, 'integer': int}
    if 'type' in schema:
        require(type(value) is types[schema['type']], f'{where}: wrong type')
    if 'const' in schema:
        require(type(value) is type(schema['const']) and value == schema['const'],
                f'{where}: wrong constant')
    if 'enum' in schema:
        require(value in schema['enum'], f'{where}: unknown enum')
    if isinstance(value, dict):
        require(set(schema.get('required', [])) <= set(value), f'{where}: missing field')
        properties = schema.get('properties', {})
        if schema.get('additionalProperties') is False:
            require(set(value) <= set(properties), f'{where}: unknown field')
        for key, child in value.items():
            if key in properties:
                schema_check(child, properties[key], f'{where}.{key}')
    if isinstance(value, list):
        require(len(value) >= schema.get('minItems', 0), f'{where}: empty array')
        for index, child in enumerate(value):
            schema_check(child, schema['items'], f'{where}[{index}]')
    if isinstance(value, str):
        require(len(value) >= schema.get('minLength', 0), f'{where}: empty string')
        if 'pattern' in schema:
            require(re.search(schema['pattern'], value) is not None, f'{where}: pattern')
    if type(value) is int and 'minimum' in schema:
        require(value >= schema['minimum'], f'{where}: below minimum')


def file_reference(path):
    target = (ROOT / path).resolve()
    require(target.is_relative_to(ROOT) and target.is_file(), f'Invalid reference: {path}')
    return target


def validate_successful_run(run, evidence):
    require(evidence.get('commit') == run['commit'], 'CI commit mismatch')
    require(evidence.get('run_id') == run['run_id'], 'CI run mismatch')
    jobs = evidence.get('jobs', [])
    require(len(jobs) == run['expected_jobs'] == run['verified_successful_jobs'],
            'Incomplete successful CI run')
    require(len({job['id'] for job in jobs}) == len(jobs), 'Duplicate CI job')
    require(all(job['conclusion'] == 'success' for job in jobs), 'Failed CI job')
    expected = set()
    for platform in ('ubuntu-latest', 'windows-latest', 'macos-latest'):
        for configuration in ('Debug', 'Release'):
            expected.add(f'build ({platform}, {configuration})')
            expected.add(f'C++ SDK / {platform} / {configuration}')
            expected.add(f'Native archived replay / {platform} / {configuration}')
    require({job['name'] for job in jobs} == expected, 'CI matrix identity mismatch')


def validate(data, schema):
    schema_check(data, schema)
    for group in ('capabilities', 'ci', 'release_gates'):
        ids = [row['id'] for row in data[group]]
        require(len(ids) == len(set(ids)), f'Duplicate {group} ID')
    for capability in data['capabilities']:
        for reference in capability['references']:
            file_reference(reference)
    inventory = json.loads(file_reference(data['catalog']['inventory']).read_text())
    require(data['catalog']['unique_part_count'] == inventory['unique_part_count'],
            'Catalog inventory count mismatch')
    for run in data['ci']:
        evidence = json.loads(file_reference(run['evidence']).read_text())
        require(run['url'] == f"https://github.com/minexin/RFSim/actions/runs/{run['run_id']}",
                'CI URL/run mismatch')
        require(run['verified_successful_jobs'] <= run['expected_jobs'], 'Impossible CI count')
        if run['status'] == 'success':
            validate_successful_run(run, evidence)


def self_test(data, schema):
    mutations = [
        lambda d: d['capabilities'][0]['references'].append('missing-evidence.json'),
        lambda d: d['capabilities'].append(copy.deepcopy(d['capabilities'][0])),
        lambda d: d['ci'][0].update(commit='0' * 40),
        lambda d: d['ci'][0].update(verified_successful_jobs=17),
        lambda d: d['ci'][0].update(run_id=0, url='https://github.com/minexin/RFSim/actions/runs/0'),
        lambda d: d['catalog'].update(unique_part_count=244),
        lambda d: d.update(overall_compatible=True),
        lambda d: d['capabilities'][0].update(vendor_evidence='compatible'),
    ]
    for mutation in mutations:
        candidate = copy.deepcopy(data)
        mutation(candidate)
        try:
            validate(candidate, schema)
        except ValueError:
            continue
        raise AssertionError('Invalid evidence mutation was accepted')
    evidence = json.loads(file_reference(data['ci'][0]['evidence']).read_text())
    evidence['jobs'][0]['name'] = 'build (unverified-platform, Release)'
    try:
        validate_successful_run(data['ci'][0], evidence)
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid CI matrix identity was accepted')
    print(f'{len(mutations) + 1} invalid-evidence mutations rejected')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    arguments = parser.parse_args()
    data = json.loads((ROOT / 'validation/compatibility-evidence.json').read_text())
    schema = json.loads((ROOT / 'validation/compatibility-evidence.schema.json').read_text())
    validate(data, schema)
    if arguments.self_test:
        self_test(data, schema)
    print('Compatibility evidence metadata valid; no numerical or vendor tests executed')

#!/usr/bin/env python3
"""Developer-only evidence metadata check; never runs or reclassifies vendor tests."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]

# These exact historical runs retain their original normalized-only evidence.
# Every new run must carry the complete REST snapshot; missing fields cannot downgrade it.
LEGACY_NORMALIZED_CI_RUN_IDS = frozenset({
    38566887480, 38568395820, 38571067267, 38572379978, 38574445472,
    38575586569, 38578171209, 38581465433, 38583782458, 38595759547,
    38597218325, 38598650770, 38579642585, 38600226155, 38607571844,
    38615005226, 38617787622, 38624386322, 38632765095, 38641741754,
    38705856824, 38706950468, 38710540300, 38720558955, 38722607277,
    38756583678, 38764883668,
})


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


def validate_ci_run(run, evidence):
    require(evidence.get('commit') == run['commit'], 'CI commit mismatch')
    require(evidence.get('run_id') == run['run_id'], 'CI run mismatch')
    require(run['expected_jobs'] == 18, 'Unexpected CI matrix size')
    jobs = evidence.get('jobs', [])
    require(len({job['id'] for job in jobs}) == len(jobs), 'Duplicate CI job')
    for job in jobs:
        require(job.get('head_sha') == run['commit'], 'CI job commit mismatch')
        require(job.get('run_id') == run['run_id'], 'CI job run mismatch')
        if run['status'] in ('success', 'failed'):
            require(job.get('status')
                    == 'completed', 'Nonterminal CI job')
            require(job.get('conclusion') in ('success', 'failure', 'cancelled'),
                    'Unexpected terminal CI conclusion')
    for field, conclusion in (('verified_successful_jobs', 'success'),
                              ('verified_failed_jobs', 'failure'),
                              ('verified_cancelled_jobs', 'cancelled')):
        require(run[field] == sum(job.get('conclusion') == conclusion for job in jobs),
                'CI conclusion count mismatch')
    if run['status'] not in ('success', 'failed'):
        return
    require(len(jobs) == run['expected_jobs'], 'Incomplete terminal CI run')
    if run['status'] == 'success':
        require(all(job['conclusion'] == 'success' for job in jobs), 'Failed CI job')
    else:
        require(run['verified_failed_jobs'] > 0, 'Failed CI run without failure')
    expected = set()
    for platform in ('ubuntu-latest', 'windows-latest', 'macos-latest'):
        for configuration in ('Debug', 'Release'):
            expected.add(f'build ({platform}, {configuration})')
            expected.add(f'C++ SDK / {platform} / {configuration}')
            expected.add(f'Native archived replay / {platform} / {configuration}')
    require(len(jobs) == len(expected), 'Duplicate CI matrix member')
    require({job['name'] for job in jobs} == expected, 'CI matrix identity mismatch')


def validate_raw_ci(run, evidence, raw_bytes):
    """Bind new summaries to complete captured REST bytes; keep older formats intact."""
    require(evidence.get('evidence_format') == 'rest_run_jobs_v1',
            'Unknown or missing raw CI evidence format')
    digest = hashlib.sha256(raw_bytes).hexdigest()
    require(digest == run['raw_snapshot_sha256'], 'Raw CI snapshot hash mismatch')
    require(evidence.get('source_snapshot_sha256') == digest,
            'CI summary/raw hash mismatch')
    raw = json.loads(raw_bytes)
    source_run = raw.get('run', {})
    source_jobs = raw.get('jobs', {})
    require(source_run.get('id') == run['run_id'], 'Raw CI run mismatch')
    require(source_run.get('head_sha') == run['commit'], 'Raw CI commit mismatch')
    require(source_run.get('html_url') == run['url'], 'Raw CI URL mismatch')
    require(source_run.get('path') == '.github/workflows/build.yml',
            'Raw CI workflow mismatch')
    require(source_run.get('status') == 'completed', 'Nonterminal raw CI run')
    require(source_run.get('conclusion') == {'success': 'success', 'failed': 'failure'}
            .get(run['status']), 'Raw CI conclusion mismatch')
    jobs = source_jobs.get('jobs', [])
    require(type(source_jobs.get('total_count')) is int
            and source_jobs['total_count'] == len(jobs), 'Raw CI total count mismatch')
    validate_ci_run(run, {'commit': source_run['head_sha'],
                         'run_id': source_run['id'], 'jobs': jobs})
    fields = ('id', 'name', 'conclusion', 'completed_at', 'head_sha', 'run_id', 'status')
    summaries = {job['id']: job for job in evidence['jobs']}
    require(set(summaries) == {job['id'] for job in jobs}, 'Raw/summary CI job IDs mismatch')
    for job in jobs:
        require(all(field in job and field in summaries[job['id']]
                    and job[field] == summaries[job['id']][field] for field in fields),
                'Raw/summary CI job fields mismatch')


def validate(data, schema):
    schema_check(data, schema)
    for group in ('capabilities', 'ci', 'release_gates'):
        ids = [row['id'] for row in data[group]]
        require(len(ids) == len(set(ids)), f'Duplicate {group} ID')
    run_ids = [run['run_id'] for run in data['ci']]
    require(len(run_ids) == len(set(run_ids)), 'Duplicate CI run ID')
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
        validate_ci_run(run, evidence)
        raw_fields = {'raw_snapshot', 'raw_snapshot_sha256'} & set(run)
        require(run['run_id'] in LEGACY_NORMALIZED_CI_RUN_IDS or len(raw_fields) == 2,
                'New CI run requires raw snapshot binding')
        require(not raw_fields or len(raw_fields) == 2, 'Incomplete raw CI binding')
        require('evidence_format' not in evidence or len(raw_fields) == 2,
                'Raw CI evidence cannot downgrade to legacy format')
        if raw_fields:
            validate_raw_ci(run, evidence, file_reference(run['raw_snapshot']).read_bytes())


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
        lambda d: d['ci'].append(dict(d['ci'][0], id='duplicate_run_with_new_id')),
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
        validate_ci_run(data['ci'][0], evidence)
    except ValueError:
        pass
    else:
        raise AssertionError('Invalid CI matrix identity was accepted')
    for run_under_test in (data['ci'][0], next(row for row in data['ci']
                                              if row['status'] == 'failed')):
        original = json.loads(file_reference(run_under_test['evidence']).read_text())
        job_mutations = [
            lambda e: e['jobs'][0].update(head_sha='0' * 40),
            lambda e: e['jobs'][0].update(run_id=0),
            lambda e: e['jobs'][0].update(status='in_progress'),
            lambda e: e['jobs'][0].update(conclusion='skipped'),
            lambda e: e['jobs'].pop(),
            lambda e: e.update(commit='0' * 40),
            lambda e: e.update(run_id=0),
        ]
        for mutation in job_mutations:
            evidence = copy.deepcopy(original)
            mutation(evidence)
            try:
                validate_ci_run(run_under_test, evidence)
            except ValueError:
                continue
            raise AssertionError('Invalid CI evidence was accepted')
        for field in ('verified_successful_jobs', 'verified_failed_jobs',
                      'verified_cancelled_jobs', 'expected_jobs'):
            run = copy.deepcopy(run_under_test)
            run[field] += 1
            try:
                validate_ci_run(run, original)
            except ValueError:
                continue
            raise AssertionError('Invalid CI count was accepted')
        evidence = copy.deepcopy(original)
        duplicate = copy.deepcopy(next(job for job in evidence['jobs']
                                       if job['conclusion'] == 'success'))
        duplicate['id'] = max(job['id'] for job in evidence['jobs']) + 1
        evidence['jobs'].append(duplicate)
        run = copy.deepcopy(run_under_test)
        run['expected_jobs'] += 1
        run['verified_successful_jobs'] += 1
        try:
            validate_ci_run(run, evidence)
        except ValueError:
            pass
        else:
            raise AssertionError('Duplicate CI matrix member with adjusted counts was accepted')
    print(f'{len(mutations) + 1 + 2 * (len(job_mutations) + 5)} invalid-evidence mutations rejected')
    raw_run = next(row for row in data['ci'] if 'raw_snapshot' in row)
    raw_evidence = json.loads(file_reference(raw_run['evidence']).read_text())
    raw_bytes = file_reference(raw_run['raw_snapshot']).read_bytes()
    raw_original = json.loads(raw_bytes)
    raw_mutations = [
        lambda r: r['run'].update(head_sha='0' * 40),
        lambda r: r['run'].update(id=0),
        lambda r: r['run'].update(status='in_progress'),
        lambda r: r['run'].update(conclusion='failure'),
        lambda r: r['run'].update(html_url='https://github.com/incorrect'),
        lambda r: r['run'].update(path='.github/workflows/other.yml'),
        lambda r: r['jobs'].update(total_count=19),
        lambda r: r['jobs']['jobs'][0].update(head_sha='0' * 40),
        lambda r: r['jobs']['jobs'][0].update(run_id=0),
        lambda r: r['jobs']['jobs'][0].update(status='queued'),
        lambda r: r['jobs']['jobs'][0].update(conclusion='failure'),
        lambda r: r['jobs']['jobs'][0].update(name='unverified job'),
        lambda r: r['jobs']['jobs'][0].update(id=0),
        lambda r: r['jobs']['jobs'][0].update(completed_at='2000-01-01T00:00:00Z'),
        lambda r: r['jobs']['jobs'].pop(),
    ]
    for mutation in raw_mutations:
        raw = copy.deepcopy(raw_original)
        mutation(raw)
        payload = json.dumps(raw).encode()
        digest = hashlib.sha256(payload).hexdigest()
        run = dict(raw_run, raw_snapshot_sha256=digest)
        evidence = dict(raw_evidence, source_snapshot_sha256=digest)
        try:
            validate_raw_ci(run, evidence, payload)
        except ValueError:
            continue
        raise AssertionError('Invalid raw CI mutation with updated digest was accepted')
    for run, evidence, payload in (
            (raw_run, raw_evidence, raw_bytes + b'\n'),
            (dict(raw_run, raw_snapshot_sha256='0' * 64), raw_evidence, raw_bytes),
            (raw_run, dict(raw_evidence, source_snapshot_sha256='0' * 64), raw_bytes)):
        try:
            validate_raw_ci(run, evidence, payload)
        except ValueError:
            continue
        raise AssertionError('Invalid raw CI hash binding was accepted')
    for field in ('raw_snapshot', 'raw_snapshot_sha256'):
        candidate = copy.deepcopy(data)
        next(row for row in candidate['ci'] if row['id'] == raw_run['id']).pop(field)
        try:
            validate(candidate, schema)
        except ValueError:
            continue
        raise AssertionError('Partial raw CI binding was accepted')
    for target in (row for row in data['ci'] if 'raw_snapshot' in row):
        candidate = copy.deepcopy(data)
        changed = next(row for row in candidate['ci'] if row['id'] == target['id'])
        changed.pop('raw_snapshot')
        changed.pop('raw_snapshot_sha256')
        try:
            validate(candidate, schema)
        except ValueError:
            continue
        raise AssertionError('New raw CI run downgraded to legacy format')
    for format_value in (None, 'legacy'):
        evidence = dict(raw_evidence)
        if format_value is None:
            evidence.pop('evidence_format')
        else:
            evidence['evidence_format'] = format_value
        try:
            validate_raw_ci(raw_run, evidence, raw_bytes)
        except ValueError:
            continue
        raise AssertionError('Invalid raw CI format marker was accepted')
    raw_count = sum('raw_snapshot' in row for row in data['ci'])
    print(f'{len(raw_mutations) + 7 + raw_count} raw-CI binding mutations rejected')


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

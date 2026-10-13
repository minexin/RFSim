"""Archive the completed seven-call study; never connect to or invoke SystemVue.

Committed verification checks the embedded execution sources and linked records.
Full verification additionally reopens every retained local artifact and rebuilds
the normalized observations. Vendor workspace bytes are not embedded here.
"""

import argparse
import copy
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import xml.parsers.expat


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


normalizer = module('archive-am-pm-datapoints-zero-study.py')
scorer = module('diagnose-am-pm-datapoints-zero-study.py')
require, load, digest = scorer.require, scorer.load, scorer.digest
parse_utc = normalizer.archive.parse_utc
BASE = 'build-reference/ampm-datapoints-zero-study-20261013'
REPAIRS = 'validation/am-pm-datapoints-zero-workflow-repairs-20261013.json'
RESTORATION = 'validation/am-pm-datapoints-zero-restoration-20261013.json'
PROOF_HASHES = {REPAIRS: 'ce959105fc4a4bd526e9ab63b9a1486c6e0685ed5492d5543b71a34ce67f7741',
                RESTORATION: 'e2930caa3699062fbb8f026e5456e1a9b25bb9078618e97cb0e349a3dd520e5d'}
CAPTURE_FILES = {'capture.json', 'normalized.json', 'partial-score.json', 'before.json',
                 'completion.json', 'closed.json', 'run-intent.json', 'application.json',
                 'source-snapshot.json', 'RFModel_AMPM_A01-result.wsv', 'stdout.log',
                 'stderr.log', 'status.json'}
PARAMETER_FIELDS = ('data', 'data_entry', 'dimensions', 'evaluation_error', 'variables', 'methods')


def sha_bytes(payload):
    return hashlib.sha256(payload).hexdigest()


def canonical(value):
    return sha_bytes(json.dumps(value, sort_keys=True, separators=(',', ':'),
                                allow_nan=False).encode('utf-8'))


def payload(value):
    return (json.dumps(value, indent=2, allow_nan=False) + '\n').encode('utf-8')


def relative(path):
    path = str(path).replace('\\', '/')
    parts = PurePosixPath(path)
    require(not parts.is_absolute() and all(p not in ('.', '..') and ':' not in p for p in parts.parts),
            'Artifact path must remain relative to its root')
    return parts.as_posix()


def local(root, path):
    target = (Path(root) / relative(path)).resolve()
    require(target.is_relative_to(Path(root).resolve()), 'Artifact escaped root')
    return target


def check_sha(value):
    require(isinstance(value, str) and re.fullmatch('[0-9a-f]{64}', value), 'Invalid SHA-256')


def source_snapshot(document):
    require(isinstance(document, dict) and document, 'Missing execution source snapshots')
    for path, source in document.items():
        relative(path)
        check_sha(source['sha256'])
        require(sha_bytes(source['utf8_source'].encode('utf-8')) == source['sha256'],
                'Embedded execution source bytes changed')


def parameters(nodes):
    prefixes = tuple('RFModel_AMPM_A01/' + suffix for suffix in (
        'DataPoints/Sch2/PartList/RFAmp/ParamSet/', 'DataPoints/Sch2/PartList/Source/ParamSet/',
        'DataPoints/System2/Path1/', 'PointDerivative/Sch1/PartList/RFAmp/ParamSet/',
        'PointDerivative/Sch1/PartList/Source/ParamSet/', 'PointDerivative/System1/Path1/'))
    result = {}
    for node in nodes:
        if node['path'].startswith(prefixes):
            result.setdefault(node['path'], []).append({field: node.get(field) for field in PARAMETER_FIELDS})
    require(len(result) == 245 and sum(map(len, result.values())) == 247, 'Incomplete restoration snapshot')
    for mode, system in (('DataPoints', 'System2'), ('PointDerivative', 'System1')):
        path = 'RFModel_AMPM_A01/' + mode + '/' + system + '/Path1/DigNumAvg'
        require([item['data'] for item in result[path]] == [10, 0], 'Duplicate parameter occurrence lost')
    return result


def xml_byte_edits(raw, changes):
    """Independently reproduce only the declared XML text spans."""
    targets = {item['path']: item for item in changes}
    require(len(targets) == len(changes) == 17, 'Exactly 17 distinct workspace edits required')
    spans, active, stack = {}, {}, []
    parser = xml.parsers.expat.ParserCreate()

    def start(tag, attrs):
        stack.append(attrs.get('XName', tag) if stack else '')
        key = '/'.join(stack[1:])
        if key in targets:
            active[key] = parser.CurrentByteIndex, tag

    def end(tag):
        key = '/'.join(stack[1:])
        if key in active:
            begin, opening = active[key]
            tag_end = raw.index(b'>', begin) + 1
            empty = raw[begin:tag_end].rstrip().endswith(b'/>')
            stop = tag_end if empty else raw.index(b'>', parser.CurrentByteIndex) + 1
            before = None if empty else raw[tag_end:parser.CurrentByteIndex].decode('utf-8')
            require(before == targets[key]['before'], 'Workspace edit before-value changed')
            require(key not in spans, 'Duplicate XML edit target')
            replacement = ('<' + opening + '>' + targets[key]['after'] + '</' + opening + '>').encode('utf-8')
            spans[key] = begin, stop, replacement
        stack.pop()

    parser.StartElementHandler, parser.EndElementHandler = start, end
    parser.Parse(raw, True)
    require(set(spans) == set(targets), 'Missing XML byte edit')
    for begin, stop, replacement in sorted(spans.values(), reverse=True):
        raw = raw[:begin] + replacement + raw[stop:]
    return raw


def collect(root, plan_path):
    root = Path(root).resolve()
    require(digest(plan_path) == scorer.PLAN_SHA256, 'Frozen plan bytes changed')
    plan = load(plan_path)
    scorer.validate_plan(plan)
    base = local(root, BASE)
    proofs = {}
    for path, expected in PROOF_HASHES.items():
        require(digest(local(root, path)) == expected, 'Workflow proof bytes changed')
        proofs[path] = load(local(root, path))
    for path, expected in proofs[REPAIRS]['source_hashes'].items():
        require(digest(local(root, path)) == expected, 'Workflow repair source bytes changed')
    prepared_path = base / 'prepared-workspaces.json'
    prepared = load(prepared_path)
    require(prepared['plan_sha256'] == scorer.PLAN_SHA256, 'Prepared plan changed')
    pristine = local(root, prepared['source_path'])
    require(digest(pristine) == prepared['source_sha256'] == plan['source_workspace_sha256'],
            'Pristine source changed')
    require([p['name'] for p in prepared['profiles']] == [p['name'] for p in plan['profiles']],
            'Prepared profile order changed')
    for item in prepared['profiles']:
        workspace = local(root, item['workspace_path'])
        require(digest(workspace) == item['workspace_sha256'], 'Prepared workspace changed')
        require(xml_byte_edits(pristine.read_bytes(), item['changes']) == workspace.read_bytes(),
                'Unregistered workspace byte edits')

    snapshots = {}

    def record_directory(directory):
        validated_path = directory / 'validated.json'
        validation = load(validated_path)
        require(relative(validation['directory']) == directory.relative_to(root).as_posix(), 'Directory identity mismatch')
        for name, expected in validation['artifact_sha256'].items():
            require('/' not in relative(name), 'Artifact name is not a basename')
            require(digest(directory / name) == expected, 'Raw artifact bytes changed: ' + name)
        snapshot_path = directory / 'source-snapshot.json'
        raw_text = snapshot_path.read_bytes().decode('utf-8')
        source_snapshot(json.loads(raw_text))
        snapshots[digest(snapshot_path)] = {'utf8_json': raw_text}
        return dict(directory=directory.relative_to(root).as_posix(), validated_sha256=digest(validated_path),
                    validation=validation, status=load(directory/'status.json'),
                    application=load(directory/'application.json'))

    parks, restores = list(base.glob('park-*')), list(base.glob('restore-*'))
    require(len(parks) == len(restores) == 1, 'Unexpected park/restore invocation count')
    park, restore = record_directory(parks[0]), record_directory(restores[0])
    original_snapshot = parameters(load(parks[0]/'before.json')['nodes'])
    require(original_snapshot == load(parks[0]/'parameter-snapshot.json'), 'Park snapshot differs from raw nodes')
    restoration = proofs[RESTORATION]
    for path, expected in restoration['artifact_sha256'].items():
        require(digest(local(root, path)) == expected, 'Restoration source bytes changed')
    source_snapshot(restoration['source_snapshots'])
    reinspection = local(root, restoration['reinspection_directory'])
    require(load(reinspection/'status.json')['exit_code'] == 0, 'Read-only reinspection failed')
    require(load(reinspection/'application.json') == park['application'], 'Reinspection application changed')
    require(parameters(load(reinspection/'restored.json')['nodes']) == original_snapshot,
            'Final complete restoration failed')
    initial = parameters(load(restores[0]/'restored.json')['nodes'])
    differences = []
    for path, items in original_snapshot.items():
        for index, (before, after) in enumerate(zip(items, initial[path])):
            require({k: v for k, v in before.items() if k != 'variables'} ==
                    {k: v for k, v in after.items() if k != 'variables'}, 'Initial parameter values differ')
            if before['variables'] != after['variables']:
                differences.append(dict(path=path, occurrence=index, before=before['variables'], after=after['variables']))
    require(differences == restoration['initial_metadata_differences'] and len(differences) == 117,
            'Initial metadata failure record changed')

    directories = sorted(base.glob('capture-*'))
    require(len(directories) == len(plan['profiles']) == 7, 'Exactly seven acquisition attempts required')
    rows, samples = [], []
    for index, (directory, profile) in enumerate(zip(directories, plan['profiles'])):
        row = record_directory(directory)
        row.update(profile=profile['name'], profile_index=index,
                   intent=load(directory/'run-intent.json'), completion=load(directory/'completion.json'),
                   closed=load(directory/'closed.json'))
        expected_files = CAPTURE_FILES | ({'baseline-comparison.json'} if index == 0 else set())
        require(set(row['validation']['artifact_sha256']) == expected_files, 'Incomplete capture artifact set')
        sample = normalizer.normalize(load(directory/'capture.json'), profile)
        require(sample == load(directory/'normalized.json'), 'Raw normalization changed')
        samples.append(sample)
        require(scorer.score(plan, samples, allow_partial=True) == load(directory/'partial-score.json'),
                'Saved partial diagnosis changed')
        row['sample_sha256'] = canonical(sample)
        rows.append(row)
    references = {}
    for name, reference in scorer.REFERENCES.items():
        path = local(root, reference['path'])
        require(digest(path) == reference['sha256'], 'Baseline bytes changed')
        references[name] = load(path)
    baseline = scorer.compare_baseline(plan, samples[0], references['ordinary_observations'], references['quick_sweep_evidence'])
    require(baseline == load(directories[0]/'baseline-comparison.json') and baseline['all_comparisons_matched'],
            'Initial baseline gate failed')
    manifest = dict(schema_version=1, classification='completed_preregistered_datapoints_zero_acquisition',
                    plan_sha256=scorer.PLAN_SHA256, base_directory=BASE,
                    analysis_calls=7, reserved_analysis_calls_used=0, planned_sweep_points=387,
                    ordinary_points=7, extra_sweep_points=7, prepared=prepared,
                    prepared_manifest_sha256=digest(prepared_path), application=park['application'],
                    park=park, captures=rows, restore=restore, source_snapshots=snapshots,
                    workflow_proof_sha256=PROOF_HASHES, baseline=baseline,
                    raw_source_bytes_verified_at_collection=True, vendor_workspace_bytes_embedded=False)
    observations = dict(schema_version=1, plan_sha256=scorer.PLAN_SHA256,
                        capture_manifest_sha256=sha_bytes(payload(manifest)), samples=[])
    for sample, row in zip(samples, rows):
        sample = copy.deepcopy(sample)
        sample['capture_source'] = dict(directory=row['directory'], capture_sha256=row['validation']['artifact_sha256']['capture.json'],
                                       normalized_sha256=row['validation']['artifact_sha256']['normalized.json'],
                                       sample_sha256=row['sample_sha256'])
        observations['samples'].append(sample)
    return manifest, observations


def verify_committed(root, plan_path, manifest_path, observations_path):
    """Verify committed links only; this never claims current raw-byte access."""
    require(digest(plan_path) == scorer.PLAN_SHA256, 'Frozen plan bytes changed')
    plan, manifest, observations = load(plan_path), load(manifest_path), load(observations_path)
    scorer.validate_plan(plan)
    require(manifest['schema_version'] == observations['schema_version'] == 1, 'Wrong archive schema')
    require(manifest['plan_sha256'] == observations['plan_sha256'] == scorer.PLAN_SHA256,
            'Archive plan identity changed')
    require(manifest['base_directory'] == BASE, 'Archive base changed')
    require(observations['capture_manifest_sha256'] == digest(manifest_path), 'Capture manifest identity changed')
    for field, expected in dict(analysis_calls=7, reserved_analysis_calls_used=0,
                                planned_sweep_points=387, ordinary_points=7, extra_sweep_points=7).items():
        require(type(manifest[field]) is int and manifest[field] == expected, 'Acquisition counts changed')
    require(manifest['raw_source_bytes_verified_at_collection'] is True and
            manifest['vendor_workspace_bytes_embedded'] is False, 'Archive scope changed')
    require(manifest['workflow_proof_sha256'] == PROOF_HASHES, 'Workflow proof identity changed')
    proofs = {}
    for path, expected in PROOF_HASHES.items():
        require(digest(local(root, path)) == expected, 'Workflow proof bytes changed')
        proofs[path] = load(local(root, path))
    for reference in scorer.REFERENCES.values():
        require(digest(local(root, reference['path'])) == reference['sha256'], 'Baseline bytes changed')
    restoration = proofs[RESTORATION]
    require(restoration['full_rich_snapshot_equal'] is True and restoration['additional_analysis_calls'] == 0
            and restoration['parameter_paths_verified'] == 245
            and restoration['parameter_occurrences_verified'] == 247
            and restoration['initial_metadata_difference_count'] == 117
            and len(restoration['initial_metadata_differences']) == 117,
            'Restoration outcome changed')
    source_snapshot(restoration['source_snapshots'])
    prepared = manifest['prepared']
    require(prepared['plan_sha256'] == scorer.PLAN_SHA256
            and prepared['source_sha256'] == plan['source_workspace_sha256'], 'Prepared source identity changed')
    require([item['name'] for item in prepared['profiles']] == [p['name'] for p in plan['profiles']],
            'Prepared profile collection changed')
    require(sha_bytes(payload(prepared)) == manifest['prepared_manifest_sha256'],
            'Prepared manifest content changed')
    snapshots = {}
    for expected, document in manifest['source_snapshots'].items():
        check_sha(expected)
        require(sha_bytes(document['utf8_json'].encode('utf-8')) == expected, 'Source snapshot document changed')
        snapshots[expected] = json.loads(document['utf8_json'])
        source_snapshot(snapshots[expected])
    require(bool(snapshots), 'Missing source snapshots')
    application = manifest['application']
    require(application['file_version'] == application['product_version'] == '2023.0.0.11903',
            'Vendor version changed')
    require(type(application['pid']) is int and application['pid'] > 0, 'Invalid vendor process')
    used_snapshots = set()

    def check_record(row, mode):
        require(relative(row['directory']).startswith(BASE + '/' + mode + '-'), 'Wrong invocation directory')
        validation, status = row['validation'], row['status']
        require(relative(validation['directory']) == row['directory'], 'Validation directory changed')
        require(sha_bytes(payload(validation)) == row['validated_sha256'], 'Validation record content changed')
        for name, expected in validation['artifact_sha256'].items():
            require('/' not in relative(name), 'Invalid artifact filename')
            check_sha(expected)
        require(sha_bytes(payload(status)) == validation['artifact_sha256']['status.json'],
                'Execution status content differs from recorded raw artifact')
        require(row['application'] == application, 'Vendor application identity changed')
        require(status['mode'] == mode and status['plan_sha256'] == scorer.PLAN_SHA256,
                'Wrong execution status identity')
        require(type(status['exit_code']) is int and status['exit_code'] == 0
                and status['status'] == 'returned' and status['timeout_seconds'] == 120,
                'Capture helper did not complete')
        wanted_calls = 1 if mode == 'capture' else 0
        require(type(status['planned_analysis_calls']) is int and status['planned_analysis_calls'] == wanted_calls,
                'Unregistered analysis call')
        require(parse_utc(status['started_utc']) <= parse_utc(status['finished_utc'])
                <= parse_utc(validation['validated_utc']), 'Invalid validation chronology')
        snapshot_hash = validation['artifact_sha256']['source-snapshot.json']
        require(snapshot_hash in snapshots, 'Missing linked execution source snapshot')
        used_snapshots.add(snapshot_hash)
        helper = [value for path, value in snapshots[snapshot_hash].items()
                  if relative(path).endswith('/' + mode + '.ps1')]
        require(len(helper) == 1 and helper[0]['sha256'] == status['script_sha256'],
                'Executed helper is not the archived source')
        return validation, status

    park, restore = manifest['park'], manifest['restore']
    pv, ps = check_record(park, 'park')
    rv, rs = check_record(restore, 'restore')
    require(pv['analysis_calls'] == rv['analysis_calls'] == 0
            and pv['parameters_preserved'] == rv['parameters_verified'] == 247
            and pv['parameter_paths_preserved'] == 245, 'Restoration coverage changed')
    require(relative(pv['validation_recovery']['path']) == REPAIRS
            and pv['validation_recovery']['sha256'] == PROOF_HASHES[REPAIRS]
            and relative(rv['followup_read_only_verification']['path']) == RESTORATION
            and rv['followup_read_only_verification']['sha256'] == PROOF_HASHES[RESTORATION],
            'Workflow recovery link changed')
    require(ps['post_capture_validation_error'] == 'Duplicate baseline parameter'
            and rs['post_capture_validation_error'] == 'Pristine parameter values/expressions/shape were not restored',
            'Initial workflow failures must remain visible')
    rows, samples = manifest['captures'], observations['samples']
    require(len(rows) == len(samples) == 7, 'Incomplete capture collection')
    scorer.validate_samples(plan, samples)
    previous = parse_utc(pv['validated_utc'])
    directories = set()
    for index, (row, sample, profile, workspace) in enumerate(zip(rows, samples, plan['profiles'], prepared['profiles'])):
        validation, status = check_record(row, 'capture')
        require(row['directory'] not in directories, 'Reused capture directory')
        directories.add(row['directory'])
        require(row['profile'] == validation['profile'] == status['profile'] == profile['name']
                and type(row['profile_index']) is int and row['profile_index'] == validation['profile_index'] == index,
                'Capture profile order changed')
        require(validation['plan_sha256'] == scorer.PLAN_SHA256, 'Capture plan changed')
        require('post_capture_validation_error' not in status, 'Failed capture cannot enter this study')
        require(set(validation['artifact_sha256']) == CAPTURE_FILES | ({'baseline-comparison.json'} if index == 0 else set()),
                'Incomplete capture artifact set')
        require(validation['planned_points'] == profile['expected_points'] and validation['extra_points'] == 1
                and len(sample['additional_sweep_points']) == 1, 'Observed point coverage changed')
        require(len(workspace['changes']) == len({r['path'] for r in workspace['changes']}) == 17,
                'Workspace edit coverage changed')
        relative(workspace['workspace_path'])
        check_sha(workspace['workspace_sha256'])
        completion, intent, closed = row['completion'], row['intent'], row['closed']
        require(intent['profile'] == profile['name'] and intent['analysis_target'] == plan['analysis_target']
                and intent['plan_sha256'] == scorer.PLAN_SHA256 and intent['analysis_calls'] == 1
                and intent['validated_parameter_count'] == 17, 'Invalid analysis intent')
        require(type(completion['analysis_done']) is int and completion['analysis_done'] == 1
                and completion['manager_errors'] == '', 'Analysis incomplete or diagnostic')
        require(intent['run_started_utc'] == completion['run_started_utc'] == sample['run_started_utc']
                and completion['run_returned_utc'] == sample['run_returned_utc'], 'Analysis time identity changed')
        require(closed['workspace_count'] == 0 and closed['input_sha256'] == workspace['workspace_sha256'],
                'Workspace closure or input bytes changed')
        times = [previous, parse_utc(status['started_utc']), parse_utc(completion['run_started_utc']),
                 parse_utc(completion['run_returned_utc']), parse_utc(closed['observed_utc']),
                 parse_utc(status['finished_utc']), parse_utc(validation['validated_utc'])]
        require(times == sorted(times), 'Next call started before predecessor validation or completion')
        previous = times[-1]
        normalized = {key: value for key, value in sample.items() if key != 'capture_source'}
        require(canonical(normalized) == row['sample_sha256'], 'Observed sample content changed')
        require(sha_bytes(payload(normalized)) == validation['artifact_sha256']['normalized.json'],
                'Normalized sample differs from recorded source artifact')
        require(sample['capture_source'] == dict(directory=row['directory'],
                capture_sha256=validation['artifact_sha256']['capture.json'],
                normalized_sha256=validation['artifact_sha256']['normalized.json'],
                sample_sha256=row['sample_sha256']), 'Sample raw-source link changed')
    require(previous <= parse_utc(rs['started_utc']), 'Restoration occurred before final capture validated')
    require(set(snapshots) == used_snapshots, 'Unlinked source snapshot')
    baseline = manifest['baseline']
    require(baseline['comparison_count'] == len(baseline['rows']) == 12
            and baseline['all_comparisons_matched'] is True
            and all(item['matched'] is True for item in baseline['rows']), 'Baseline gate failed')
    require({key: value['count'] for key, value in baseline['categories'].items()} == {
        'same_mode_ordinary_reproduction': 2, 'cross_mode_sweep_to_ordinary': 7,
        'same_path_different_sweep_mode': 3}, 'Baseline identities changed')
    reproduced = scorer.compare_baseline(plan, samples[0],
        load(local(root, scorer.REFERENCES['ordinary_observations']['path'])),
        load(local(root, scorer.REFERENCES['quick_sweep_evidence']['path'])))
    require(baseline == reproduced, 'Baseline numerical record differs from verified observations')
    return dict(committed_archive_verified=True, raw_source_bytes_verified=False,
                capture_count=7, planned_sweep_points=387, ordinary_points=7, extra_sweep_points=7)


def verify_archive(root, plan_path, manifest_path, observations_path):
    result = verify_committed(root, plan_path, manifest_path, observations_path)
    manifest, observations = collect(root, plan_path)
    require(manifest == load(manifest_path), 'Raw acquisition archive differs from committed manifest')
    require(observations == load(observations_path), 'Raw observations differ from committed evidence')
    return dict(result, raw_source_bytes_verified=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('plan', type=Path)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('observations', type=Path)
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    if args.verify_only:
        print(json.dumps(verify_archive(args.root, args.plan, args.manifest, args.observations)))
        return
    require(not args.manifest.exists() and not args.observations.exists(), 'Archive output exists')
    manifest, observations = collect(args.root, args.plan)
    for path, value in ((args.manifest, manifest), (args.observations, observations)):
        with path.open('xb') as stream:
            stream.write(payload(value))
    print(json.dumps(verify_archive(args.root, args.plan, args.manifest, args.observations)))


if __name__ == '__main__':
    main()

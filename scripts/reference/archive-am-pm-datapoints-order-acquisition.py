"""Archive the frozen ten-call order study without invoking SystemVue.

Committed verification checks recorded evidence and embedded execution sources.
Full verification also reopens the retained local captures and workspaces.
"""

import argparse
import copy
import importlib.util
import json
import math
from pathlib import Path


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


raw_axis = module('archive-am-pm-datapoints-raw-axis-acquisition.py')
previous = raw_axis.previous
compare_replay = raw_axis.compare_replay
preparer = module('prepare-am-pm-datapoints-order-study.py')
normalizer = previous.normalizer
require, load, digest = previous.require, previous.load, previous.digest
sha_bytes, payload, canonical = previous.sha_bytes, previous.payload, previous.canonical
relative, local, check_sha = previous.relative, previous.local, previous.check_sha
parse_utc = previous.parse_utc
BASE = 'build-reference/ampm-datapoints-order-study-20261014'
WORKFLOW = 'validation/am-pm-datapoints-order-workflow-20261014.json'
PLAN_SHA256 = '3fd61758408c87d1fb3e163ceee04a8ceb9b75f77a880ae4ea3a8c0ab2e58de6'
WORKFLOW_SHA256 = '1c905eda56441863f1a1399139a2b9f9d4a70ad25e597ade8b51625b59fe933e'
REPLAY_SOURCE_FILES = (
    'prepare-am-pm-datapoints-order-study.py',
    'diagnose-am-pm-datapoints-order-study.py',
    'prepare-am-pm-datapoints-raw-axis-study.py',
    'diagnose-am-pm-datapoints-raw-axis-study.py',
    'archive-am-pm-datapoints-zero-study.py',
    'archive-am-pm-datapoints.py',
    'analyze-am-pm-datapoints.py',
    'diagnose-am-pm-datapoints-zero-study.py',
)
ENABLE = normalizer.SWEEP + 'Enable'
SOURCE_POWER = normalizer.SOURCE + 'Pwr'
RECOVERY_COPY = 'RFModel_AMPM_A01-rejected-study-copy.wsv'
CLASSIFICATION = 'completed_preregistered_near_zero_order_acquisition'
COMMON_FILES = {'application.json', 'source-snapshot.json', 'stdout.log', 'stderr.log', 'status.json'}
MODE_FILES = {
    'preflight': COMMON_FILES | {'before.json', 'parameter-snapshot.json'},
    'park': COMMON_FILES | {'before.json', 'closed.json', 'parameter-snapshot.json',
                           'RFModel_AMPM_A01-before-datapoints-order-study.wsv'},
    'restore': COMMON_FILES | {'restored.json', 'recovery-before-open.json'},
    'capture': COMMON_FILES | {'capture.json', 'normalized.json', 'partial-score.json', 'before.json',
                              'completion.json', 'closed.json', 'run-intent.json',
                              'RFModel_AMPM_A01-result.wsv'},
}


def scorer_module():
    return module('diagnose-am-pm-datapoints-order-study.py')


def comparison_files(index):
    return (['baseline-comparison.json'] if index in (0, 5) else []) + (
        ['repeated-sweep-comparison.json'] if index > 0 else [])


def artifact_files(mode, index=None, *, recovered_copy=False):
    files = set(MODE_FILES[mode])
    if mode == 'capture':
        files.update(comparison_files(index))
    elif mode == 'restore' and recovered_copy:
        files.add(RECOVERY_COPY)
    return files


def document_files(mode, index=None):
    names = ['status.json', 'application.json']
    if mode in ('park', 'capture'):
        names.append('closed.json')
    if mode == 'capture':
        names += ['run-intent.json', 'completion.json', 'partial-score.json'] + comparison_files(index)
    if mode == 'restore':
        names.append('recovery-before-open.json')
    return names


def readbacks(nodes, profile):
    """Retain all eighteen readbacks and the original source expression."""
    required = set(normalizer.expected_parameters(profile)) | {ENABLE}
    values = {}
    for node in nodes:
        if node['path'] in required:
            require(node['path'] not in values, 'Duplicate parameter readback')
            values[node['path']] = {key: copy.deepcopy(node.get(key)) for key in ('data', 'dimensions', 'data_entry')}
    require(set(values) == required, 'Incomplete eighteen-parameter readback')
    return values


def check_readbacks(values, profile):
    expected = dict(normalizer.expected_parameters(profile), **{ENABLE: 1})
    require(set(values) == set(expected), 'Incomplete eighteen-parameter readback')
    for path, wanted in expected.items():
        item = values[path]
        actual = item['data']
        if isinstance(wanted, list):
            require(isinstance(actual, list) and len(actual) == len(wanted)
                    and item['dimensions'] == [1, len(wanted)], 'Invalid table readback shape')
            pairs = zip(actual, wanted)
        else:
            require(item['dimensions'] is None, 'Invalid scalar readback shape')
            pairs = [(actual, wanted)]
        for measured, target in pairs:
            measured = normalizer.number(measured)
            require(abs(measured - target) <= 1e-12 * max(1e-12, abs(target)),
                    'Parameter readback value changed: ' + path)
    require(values[SOURCE_POWER]['data_entry'] == preparer.source_expression(
        profile['ordinary_output_target_dbm'], profile['gain_db']), 'Registered Decimal source-power expression changed')


def normalized_sources(sources):
    previous.source_snapshot(sources)
    normalized = {relative(path): value for path, value in sources.items()}
    require(len(normalized) == len(sources), 'Duplicate normalized source path')
    return normalized


def check_workflow(workflow, plan):
    require(workflow['plan_sha256'] == PLAN_SHA256, 'Workflow plan identity changed')
    sources = normalized_sources(workflow['sources'])
    expected_replay_sources = {'scripts/reference/' + name for name in REPLAY_SOURCE_FILES}
    helper_names = ('run.py', 'make-plan.py', 'make-runtime.py', 'make-workflow.py',
                    'preflight.ps1', 'park.ps1', 'capture.ps1', 'restore.ps1')
    require(set(sources) == expected_replay_sources | {BASE + '/' + name for name in helper_names},
            'Frozen sixteen-source collection changed')
    # Replay uses these sibling modules, irrespective of the evidence root.
    # Permit Git's LF/CRLF checkout conversion, but no executable source edits.
    for name in REPLAY_SOURCE_FILES:
        current = Path(__file__).resolve().with_name(name).read_bytes()
        frozen = sources['scripts/reference/' + name]['utf8_source'].encode('utf-8')
        require(current.replace(b'\r\n', b'\n') == frozen.replace(b'\r\n', b'\n'),
                'Replay source differs from frozen workflow: ' + name)
    for mode in MODE_FILES:
        require(BASE + '/' + mode + '.ps1' in sources, 'Missing frozen execution helper')
    require(BASE + '/run.py' in sources, 'Missing frozen acquisition supervisor')
    committed = [relative(path) for path in workflow['committed_sources']]
    require(len(committed) == len(set(committed)), 'Duplicate committed source declaration')
    for path, proof in workflow.get('normalized_git_sources', {}).items():
        path = relative(path)
        require(path in sources and proof['normalization'] == 'CRLF_to_LF',
                'Invalid normalized Git source declaration')
        check_sha(proof['git_sha256'])
        require(sha_bytes(sources[path]['utf8_source'].replace('\r\n', '\n').encode('utf-8'))
                == proof['git_sha256'], 'Normalized Git source identity changed')
    prepared = workflow['prepared_manifest']
    require(sha_bytes(payload(prepared)) == workflow['prepared_manifest_sha256'],
            'Prepared manifest content changed')
    require(prepared['plan_sha256'] == PLAN_SHA256
            and prepared['source_sha256'] == plan['source_workspace_sha256'],
            'Prepared source identity changed')
    require([item['name'] for item in prepared['profiles']] == [p['name'] for p in plan['profiles']],
            'Prepared profile collection changed')
    require(relative(prepared['source_path']) == preparer.SOURCE_PATH,
            'Prepared source workspace path changed')
    require(type(prepared['independent_input_directories']) is int
            and prepared['independent_input_directories'] == 10, 'Independent input count changed')
    for item, profile in zip(prepared['profiles'], plan['profiles']):
        require(relative(item['workspace_path']) == BASE + '/inputs/profiles/' + profile['name'] + '/RFModel_AMPM_A01.wsv',
                'Prepared independent input path changed')
        check_sha(item['workspace_sha256'])
        require(item['source_power_expression'] == preparer.source_expression(
            profile['ordinary_output_target_dbm'], profile['gain_db']), 'Prepared Decimal source expression changed')
        require(item['block'] == profile['block'] and item['ordinary_role'] == profile['ordinary_role']
                and type(item['block_position']) is int and item['block_position'] == profile['block_position'],
                'Prepared block or role changed')
        edits = item['changes']
        require(len(edits) == 17 and len({edit['path'] for edit in edits}) == 17,
                'Workspace edit coverage changed')
        require({edit['path']: edit['after'] for edit in edits} == preparer.planned_fields(profile),
                'Workspace changes differ from registered parameters')
    by_name = {item['name']: item for item in prepared['profiles']}
    targets = {profile['name']: profile['ordinary_output_target_dbm'] for profile in plan['profiles']}
    expected_pairs = []
    for left, right in plan['order_comparisons']['pairs']:
        require(by_name[left]['workspace_sha256'] == by_name[right]['workspace_sha256']
                and by_name[left]['changes'] == by_name[right]['changes'], 'Same-target prepared bytes differ')
        expected_pairs.append(dict(profiles=[left, right], ordinary_output_target_dbm=targets[left],
                                   workspace_sha256=by_name[left]['workspace_sha256'], byte_identical=True))
    require(canonical(prepared['same_target_pairs']) == canonical(expected_pairs),
            'Same-target preparation proof changed')
    expected_gates = dict(zero_baseline_indices=[0, 5], repeated_sweep_indices=list(range(1, 10)),
        ordinary_input_reference_indices=[0] * 5 + [5] * 5,
        descriptive_ordinary_mismatches_block_next_call=False,
        prepared_input_source_expression_readback=True, required_pre_open_workspace_count=0,
        identified_completed_rejected_copy_archived_before_restoration=True,
        unknown_or_incomplete_application_state_preserved=True,
        terminal_failed_restoration_can_be_retried_without_analysis=True,
        maximum_analysis_calls=10, reserved_calls=0)
    require(canonical(workflow['execution_gates']) == canonical(expected_gates), 'Frozen execution gates changed')
    application = workflow['expected_application']
    require(set(application) == {'pid', 'started_utc', 'executable', 'file_version', 'product_version'}
            and type(application['pid']) is int and application['pid'] > 0
            and application['file_version'] == application['product_version'] == '2023.0.0.11903',
            'Frozen application identity changed')
    parse_utc(application['started_utc'])
    historical = workflow['historical_restoration']
    relative(historical['path'])
    check_sha(historical['sha256'])
    require(sha_bytes(payload(historical['parameter_snapshot'])) == historical['parameter_snapshot_sha256'],
            'Historical ordered parameter snapshot changed')
    return sources, prepared


def collect(root, plan_path):
    root = Path(root).resolve()
    require(digest(plan_path) == PLAN_SHA256, 'Frozen plan bytes changed')
    plan, scorer = load(plan_path), scorer_module()
    scorer.validate_plan(plan)
    workflow_path = local(root, WORKFLOW)
    require(digest(workflow_path) == WORKFLOW_SHA256, 'Frozen workflow bytes changed')
    workflow = load(workflow_path)
    sources, prepared = check_workflow(workflow, plan)
    reference_path = local(root, plan['baseline_reference']['path'])
    require(digest(reference_path) == plan['baseline_reference']['sha256'], 'Historical baseline bytes changed')
    reference = load(reference_path)
    prepared_path = local(root, BASE + '/inputs/prepared-workspaces.json')
    require(digest(prepared_path) == workflow['prepared_manifest_sha256']
            and load(prepared_path) == prepared, 'Prepared input manifest changed')
    pristine = local(root, prepared['source_path'])
    require(digest(pristine) == prepared['source_sha256'], 'Pristine source bytes changed')
    pristine_bytes = pristine.read_bytes()
    for item, profile in zip(prepared['profiles'], plan['profiles']):
        workspace = local(root, item['workspace_path'])
        require(digest(workspace) == item['workspace_sha256'], 'Prepared workspace bytes changed')
        reproduced, changes = preparer.prepare_document(pristine_bytes, profile)
        require(reproduced == workspace.read_bytes() and changes == item['changes'],
                'Prepared workspace has unregistered byte edits')
    historical = workflow['historical_restoration']
    historical_path = local(root, historical['path'])
    require(digest(historical_path) == historical['sha256'], 'Historical restoration bytes changed')
    historical_parameters = previous.parameters(load(historical_path)['nodes'])
    require(historical_parameters == historical['parameter_snapshot'],
            'Historical restoration snapshot differs from retained raw file')
    base = local(root, BASE)
    snapshots = {}

    def record(directory, mode, index=None):
        validation_path = directory / 'validated.json'
        validation = load(validation_path)
        expected_files = artifact_files(mode, index, recovered_copy=(directory / RECOVERY_COPY).is_file())
        require(set(validation['artifact_sha256']) == expected_files, 'Incomplete invocation artifact set')
        require(relative(validation['directory']) == directory.relative_to(root).as_posix(),
                'Invocation directory identity changed')
        for name, expected in validation['artifact_sha256'].items():
            require('/' not in relative(name), 'Artifact filename is not a basename')
            require(digest(directory / name) == expected, 'Raw artifact bytes changed: ' + name)
        snapshot_path = directory / 'source-snapshot.json'
        text = snapshot_path.read_bytes().decode('utf-8')
        require(normalized_sources(json.loads(text.lstrip('\ufeff'))) == sources,
                'Executed source snapshot differs from frozen workflow')
        snapshots[digest(snapshot_path)] = {'utf8_json': text}
        documents = {name: (directory / name).read_bytes().decode('utf-8') for name in document_files(mode, index)}
        return dict(directory=directory.relative_to(root).as_posix(),
                    validated_sha256=digest(validation_path), validation=validation,
                    documents=documents)

    controls = {}
    for mode in ('preflight', 'park', 'restore'):
        directories = sorted(base.glob(mode + '-*'))
        require(len(directories) == 1, 'Exactly one ' + mode + ' invocation required')
        directory = directories[0]
        row = record(directory, mode)
        raw = load(directory / ('restored.json' if mode == 'restore' else 'before.json'))
        parameters = previous.parameters(raw['nodes'])
        require(parameters == historical_parameters, 'Full ordered parameter restoration differs')
        parameter_hash = sha_bytes(payload(parameters))
        if mode != 'restore':
            require(digest(directory / 'parameter-snapshot.json') == parameter_hash
                    and load(directory / 'parameter-snapshot.json') == parameters,
                    'Saved parameter snapshot differs from raw nodes')
        row['parameter_snapshot_sha256'] = parameter_hash
        row['workspace_observation'] = {key: copy.deepcopy(value) for key, value in raw.items()
                                        if key not in ('nodes', 'application')}
        controls[mode] = row
    directories = sorted(base.glob('capture-*'))
    require(len(directories) == len(plan['profiles']) == 10,
            'Exactly ten clean acquisition attempts required; failed or extra attempts require separate archival handling')
    rows, samples = [], []
    for index, (directory, profile) in enumerate(zip(directories, plan['profiles'])):
        row = record(directory, 'capture', index)
        raw = load(directory / 'capture.json')
        before_document = load(directory / 'before.json')
        before = readbacks(before_document['nodes'], profile)
        after = readbacks(raw['nodes'], profile)
        check_readbacks(before, profile)
        check_readbacks(after, profile)
        sample = normalizer.normalize(raw, profile)
        require(sample == load(directory / 'normalized.json'), 'Raw normalization differs from saved sample')
        samples.append(sample)
        compare_replay(document(row, 'partial-score.json'), scorer.score(
            plan, samples, allow_partial=True, historical_observations=reference))
        row.update(profile=profile['name'], profile_index=index, parameters_before=before,
                   parameters_after=after, sample_sha256=canonical(sample),
                   before_workspace_observation={key: copy.deepcopy(value) for key, value in before_document.items()
                                                 if key != 'nodes'},
                   workspace_names_after=copy.deepcopy(raw['workspace_names']))
        require(canonical(raw['application']) == canonical(workflow['expected_application']),
                'Capture application identity changed')
        rows.append(row)
    manifest = dict(schema_version=1, classification=CLASSIFICATION,
                    plan_sha256=PLAN_SHA256, base_directory=BASE,
                    workflow_manifest_sha256=digest(workflow_path), workflow=workflow,
                    analysis_calls=10, reserved_analysis_calls_used=0, ordinary_points=10,
                    historical_reproductions=6, new_prediction_checks=2, order_reproductions=2,
                    paired_targets=5, invocation_count=13, planned_sweep_points=610,
                    extra_sweep_points=sum(len(sample['additional_sweep_points']) for sample in samples),
                    parameter_snapshot=historical_parameters,
                    parameter_snapshot_sha256=sha_bytes(payload(historical_parameters)),
                    source_snapshots=snapshots, captures=rows,
                    raw_source_bytes_verified_at_collection=True, vendor_workspace_bytes_embedded=False,
                    **controls)
    observations = dict(schema_version=1, plan_sha256=PLAN_SHA256,
                        capture_manifest_sha256=sha_bytes(payload(manifest)), samples=[])
    for sample, row in zip(samples, rows):
        sample = copy.deepcopy(sample)
        artifacts = row['validation']['artifact_sha256']
        sample['capture_source'] = dict(directory=row['directory'], capture_sha256=artifacts['capture.json'],
                                       normalized_sha256=artifacts['normalized.json'],
                                       sample_sha256=row['sample_sha256'])
        observations['samples'].append(sample)
    _verify(root, plan, manifest, observations, sha_bytes(payload(manifest)))
    return manifest, observations


def document(row, name):
    text = row['documents'][name]
    require(sha_bytes(text.encode('utf-8')) == row['validation']['artifact_sha256'][name],
            'Embedded record differs from retained raw artifact: ' + name)
    return json.loads(text.lstrip('\ufeff'))


def check_recovery(row, status, permitted_directories):
    recovery = document(row, 'recovery-before-open.json')
    require(type(recovery['analysis_calls']) is int and recovery['analysis_calls'] == 0,
            'Restoration recovery must not run analysis')
    kind = recovery['status']
    copied = RECOVERY_COPY in row['validation']['artifact_sha256']
    if kind == 'no_open_workspace':
        expected_count = 0
        require(not copied, 'Unexpected rejected-copy archive for empty workspace recovery')
    elif kind == 'pristine_already_open':
        expected_count = 1
        require(not copied, 'Pristine workspace must not be archived as a rejected copy')
    else:
        prefix = 'archived_completed_study_copy:'
        require(isinstance(kind, str) and kind.startswith(prefix) and copied,
                'Unidentified restoration recovery action')
        directory = kind[len(prefix):].replace('\\', '/').rstrip('/')
        require('..' not in directory.split('/') and any(
            directory.lower().endswith('/' + path.lower()) for path in permitted_directories),
            'Recovered workspace directory is not a registered study input or capture')
        expected_count = 0
    require(type(recovery['workspace_count']) is int and recovery['workspace_count'] == expected_count
            and recovery['rejected_copy_archived'] is copied, 'Recovery workspace/archive state changed')
    require(parse_utc(status['started_utc']) <= parse_utc(recovery['observed_utc'])
            <= parse_utc(row['workspace_observation']['observed_utc']) <= parse_utc(status['finished_utc']),
            'Restoration recovery chronology changed')


def _verify(root, plan, manifest, observations, manifest_sha256):
    scorer = scorer_module()
    scorer.validate_plan(plan)
    require(type(manifest['schema_version']) is int and manifest['schema_version'] == 1
            and type(observations['schema_version']) is int and observations['schema_version'] == 1,
            'Wrong archive schema')
    require(manifest['plan_sha256'] == observations['plan_sha256'] == PLAN_SHA256,
            'Archive plan identity changed')
    require(manifest['base_directory'] == BASE and manifest['classification'] == CLASSIFICATION,
            'Archive base or classification changed')
    require(observations['capture_manifest_sha256'] == manifest_sha256, 'Capture manifest identity changed')
    workflow_path = local(root, WORKFLOW)
    require(digest(workflow_path) == manifest['workflow_manifest_sha256'] == WORKFLOW_SHA256
            and load(workflow_path) == manifest['workflow'], 'Frozen workflow identity changed')
    workflow = manifest['workflow']
    sources, prepared = check_workflow(workflow, plan)
    reference = plan['baseline_reference']
    reference_path = local(root, reference['path'])
    require(digest(reference_path) == reference['sha256'], 'Historical baseline bytes changed')
    prior = load(reference_path)
    for field, expected in dict(analysis_calls=10, reserved_analysis_calls_used=0, ordinary_points=10,
                                historical_reproductions=6, new_prediction_checks=2, order_reproductions=2,
                                paired_targets=5, invocation_count=13, planned_sweep_points=610).items():
        require(type(manifest[field]) is int and manifest[field] == expected, 'Acquisition counts changed')
    require(manifest['raw_source_bytes_verified_at_collection'] is True
            and manifest['vendor_workspace_bytes_embedded'] is False, 'Archive scope changed')
    parameter_snapshot = manifest['parameter_snapshot']
    require(parameter_snapshot == workflow['historical_restoration']['parameter_snapshot'],
            'Restoration differs from frozen historical parameter state')
    require(len(parameter_snapshot) == 245 and sum(map(len, parameter_snapshot.values())) == 247,
            'Incomplete ordered restoration parameters')
    parameter_hash = sha_bytes(payload(parameter_snapshot))
    require(parameter_hash == manifest['parameter_snapshot_sha256'], 'Restoration parameter snapshot changed')
    for mode, system in (('DataPoints', 'System2'), ('PointDerivative', 'System1')):
        path = 'RFModel_AMPM_A01/' + mode + '/' + system + '/Path1/DigNumAvg'
        require([item['data'] for item in parameter_snapshot[path]] == [10, 0],
                'Ordered duplicate restoration parameters changed')
    snapshots = {}
    for expected, value in manifest['source_snapshots'].items():
        check_sha(expected)
        text = value['utf8_json']
        require(sha_bytes(text.encode('utf-8')) == expected, 'Source snapshot document changed')
        snapshot = normalized_sources(json.loads(text.lstrip('\ufeff')))
        require(snapshot == sources, 'Executed source snapshot differs from frozen workflow')
        snapshots[expected] = snapshot
    require(bool(snapshots), 'Missing execution source snapshots')
    application = document(manifest['preflight'], 'application.json')
    require(canonical(application) == canonical(workflow['expected_application']),
            'Vendor process/start-time/version differs from frozen application identity')
    used_snapshots, used_directories = set(), set()

    def check_record(row, mode, index=None):
        directory = relative(row['directory'])
        require(directory.startswith(BASE + '/' + mode + '-') and directory not in used_directories,
                'Invalid or reused invocation directory')
        used_directories.add(directory)
        validation = row['validation']
        require(relative(validation['directory']) == directory and validation['plan_sha256'] == PLAN_SHA256,
                'Validation identity changed')
        require(sha_bytes(payload(validation)) == row['validated_sha256'], 'Validation record content changed')
        expected_files = artifact_files(mode, index,
            recovered_copy=RECOVERY_COPY in validation['artifact_sha256'])
        require(set(validation['artifact_sha256']) == expected_files, 'Incomplete invocation artifact set')
        require(set(row['documents']) == set(document_files(mode, index)), 'Embedded document collection changed')
        for expected in validation['artifact_sha256'].values():
            check_sha(expected)
        status = document(row, 'status.json')
        require(canonical(document(row, 'application.json')) == canonical(application),
                'Vendor application identity changed')
        require(status['mode'] == mode and status['plan_sha256'] == PLAN_SHA256,
                'Wrong execution status identity')
        require(type(status['exit_code']) is int and status['exit_code'] == 0
                and status['status'] == 'returned' and type(status['timeout_seconds']) is int
                and status['timeout_seconds'] == 120
                and 'post_capture_validation_error' not in status, 'Invocation did not complete cleanly')
        calls = 1 if mode == 'capture' else 0
        require(type(status['planned_analysis_calls']) is int and status['planned_analysis_calls'] == calls,
                'Unexpected analysis call count')
        require(parse_utc(status['started_utc']) <= parse_utc(status['finished_utc'])
                <= parse_utc(validation['validated_utc']), 'Invalid validation chronology')
        snapshot_hash = validation['artifact_sha256']['source-snapshot.json']
        require(snapshot_hash in snapshots, 'Missing linked execution source snapshot')
        used_snapshots.add(snapshot_hash)
        require(status['script_sha256'] == sources[BASE + '/' + mode + '.ps1']['sha256'],
                'Executed helper identity differs from frozen workflow')
        require(validation['artifact_sha256']['stderr.log'] == sha_bytes(b''),
                'Successful invocation contains stderr output')
        return validation, status

    preflight, park, restore = (manifest[mode] for mode in ('preflight', 'park', 'restore'))
    fv, fs = check_record(preflight, 'preflight')
    pv, ps = check_record(park, 'park')
    rv, rs = check_record(restore, 'restore')
    for record, fields in ((fv, dict(analysis_calls=0, parameters_verified=247)),
                           (pv, dict(analysis_calls=0, parameters_preserved=247, parameter_paths_preserved=245)),
                           (rv, dict(analysis_calls=0, parameters_verified=247, parameter_paths_verified=245))):
        require(all(type(record[field]) is int and record[field] == wanted for field, wanted in fields.items()),
                'Restoration coverage changed')
    require(rv['user_application_left_running'] is True, 'User application restoration changed')
    for row in (preflight, park, restore):
        require(row['parameter_snapshot_sha256'] == parameter_hash, 'Restoration states differ')
    for row in (preflight, park):
        require(row['validation']['artifact_sha256']['parameter-snapshot.json'] == parameter_hash,
                'Restoration snapshot differs from recorded raw artifact')
    for row, status in ((preflight, fs), (park, ps), (restore, rs)):
        observed = parse_utc(row['workspace_observation']['observed_utc'])
        require(parse_utc(status['started_utc']) <= observed <= parse_utc(status['finished_utc']),
                'Workspace observation outside invocation')
    for row in (preflight, restore):
        require(row['workspace_observation']['workspace_names'] == ['RFModel_AMPM_A01'],
                'Workspace control identity changed')
    require(type(preflight['workspace_observation']['analysis_calls']) is int
            and preflight['workspace_observation']['analysis_calls'] == 0
            and type(restore['workspace_observation']['analysis_calls']) is int
            and restore['workspace_observation']['analysis_calls'] == 0
            and preflight['workspace_observation']['pristine_sha256']
            == restore['workspace_observation']['pristine_sha256'] == plan['source_workspace_sha256'],
            'Pristine workspace identity changed')
    closed = document(park, 'closed.json')
    require(type(closed['workspace_count']) is int and closed['workspace_count'] == 0
            and type(closed['analysis_calls']) is int and closed['analysis_calls'] == 0
            and closed['pristine_sha256'] == plan['source_workspace_sha256'], 'Baseline was not parked')
    require(parse_utc(ps['started_utc']) <= parse_utc(park['workspace_observation']['observed_utc'])
            <= parse_utc(closed['observed_utc']) <= parse_utc(ps['finished_utc']), 'Park chronology changed')
    require(parse_utc(workflow['created_utc']) <= parse_utc(fs['started_utc'])
            and parse_utc(fv['validated_utc']) <= parse_utc(ps['started_utc']),
            'Preflight or park preceded frozen workflow or validation')
    rows, samples = manifest['captures'], observations['samples']
    require(len(rows) == len(samples) == 10, 'Incomplete capture collection')
    scorer.validate_samples(plan, samples)
    previous_time = parse_utc(pv['validated_utc'])
    extra_count = 0
    normalized_samples = []
    for index, (row, sample, profile, workspace) in enumerate(zip(rows, samples, plan['profiles'], prepared['profiles'])):
        validation, status = check_record(row, 'capture', index)
        require(row['profile'] == validation['profile'] == status['profile'] == profile['name']
                and type(row['profile_index']) is int and row['profile_index'] == index
                and type(validation['profile_index']) is int and validation['profile_index'] == index,
                'Capture profile order changed')
        extras = len(sample['additional_sweep_points'])
        require(extras in (0, 1) and type(validation['extra_points']) is int
                and validation['extra_points'] == extras and type(validation['planned_points']) is int
                and validation['planned_points'] == profile['expected_points'] == 61,
                'Observed point coverage changed')
        extra_count += extras
        for key in ('parameters_before', 'parameters_after'):
            check_readbacks(row[key], profile)
        before = row['before_workspace_observation']
        require(type(before['pre_open_workspace_count']) is int and before['pre_open_workspace_count'] == 0,
                'Capture did not begin with zero open workspaces')
        require(before['workspace_names'] == row['workspace_names_after'] == ['RFModel_AMPM_A01'],
                'Capture workspace identity changed')
        timestamps = sample['timestamps']
        require(set(timestamps) == {normalizer.PATH + '1', normalizer.PATH + '2', normalizer.RAW},
                'Incomplete normalized dataset timestamps')
        begin, end = parse_utc(sample['run_started_utc']), parse_utc(sample['run_returned_utc'])
        require(begin <= end and all(type(stamp) is int and math.floor(begin) <= stamp <= math.ceil(end)
                                    for stamp in timestamps.values()), 'Stale normalized dataset timestamp')
        require({path: item['data'] for path, item in row['parameters_after'].items() if path != ENABLE}
                == sample['parameters_si'], 'Normalized parameter values differ from readbacks')
        intent, completion, closed = (document(row, name) for name in
                                       ('run-intent.json', 'completion.json', 'closed.json'))
        require(intent['profile'] == profile['name'] and intent['analysis_target'] == plan['analysis_target']
                and intent['plan_sha256'] == PLAN_SHA256
                and type(intent['analysis_calls']) is int and intent['analysis_calls'] == 1
                and type(intent['validated_parameter_count']) is int and intent['validated_parameter_count'] == 18,
                'Invalid eighteen-parameter analysis intent')
        require(type(completion['analysis_done']) is int and completion['analysis_done'] == 1
                and completion['manager_errors'] == '', 'Analysis incomplete or diagnostic')
        require(intent['run_started_utc'] == completion['run_started_utc'] == sample['run_started_utc']
                and completion['run_returned_utc'] == sample['run_returned_utc'], 'Analysis time identity changed')
        require(type(closed['workspace_count']) is int and closed['workspace_count'] == 0
                and closed['input_sha256'] == workspace['workspace_sha256'],
                'Workspace closure or prepared input identity changed')
        times = [previous_time, parse_utc(status['started_utc']), parse_utc(before['observed_utc']),
                 parse_utc(completion['run_started_utc']),
                 parse_utc(completion['run_returned_utc']), parse_utc(closed['observed_utc']),
                 parse_utc(status['finished_utc']), parse_utc(validation['validated_utc'])]
        require(times == sorted(times), 'Next call preceded predecessor validation or completion')
        previous_time = times[-1]
        normalized = {key: value for key, value in sample.items() if key != 'capture_source'}
        require(canonical(normalized) == row['sample_sha256'], 'Observed sample content changed')
        artifacts = validation['artifact_sha256']
        require(sha_bytes(payload(normalized)) == artifacts['normalized.json'],
                'Normalized sample differs from recorded raw artifact')
        require(sample['capture_source'] == dict(directory=row['directory'], capture_sha256=artifacts['capture.json'],
                normalized_sha256=artifacts['normalized.json'], sample_sha256=row['sample_sha256']),
                'Sample raw-source link changed')
        normalized_samples.append(normalized)
        diagnosis = scorer.score(plan, normalized_samples, allow_partial=True, historical_observations=prior)
        compare_replay(document(row, 'partial-score.json'), diagnosis)
        for filename in comparison_files(index):
            comparison = scorer.compare_baseline(plan, normalized, prior) if filename == 'baseline-comparison.json' else (
                scorer.compare_repeated_sweep(plan, normalized, normalized_samples[0]))
            require(comparison['all_comparisons_matched'] is True, 'Baseline or repeated sweep control failed')
            compare_replay(document(row, filename), comparison)
    require(type(manifest['extra_sweep_points']) is int and manifest['extra_sweep_points'] == extra_count,
            'Extra endpoint count changed')
    require(previous_time <= parse_utc(rs['started_utc']), 'Restoration preceded final validated capture')
    permitted = [relative(item['workspace_path']).rsplit('/', 1)[0] for item in prepared['profiles']]
    permitted += [relative(row['directory']) for row in rows]
    check_recovery(restore, rs, permitted)
    require(set(snapshots) == used_snapshots, 'Unlinked execution source snapshot')
    require(len(used_directories) == 13, 'Invocation coverage changed')
    return dict(committed_archive_verified=True, raw_source_bytes_verified=False,
                capture_count=10, ordinary_points=10, historical_reproductions=6, new_prediction_checks=2,
                order_reproductions=2, paired_targets=5, invocation_count=13,
                planned_sweep_points=610, extra_sweep_points=extra_count,
                parameter_readbacks_per_capture=18, baseline_gates_passed=2,
                repeated_sweep_gates_passed=9, restored_parameter_paths=245,
                restored_parameter_occurrences=247, eligible_for_full_model_compatibility=False)


def verify_committed(root, plan_path, manifest_path, observations_path):
    require(digest(plan_path) == PLAN_SHA256, 'Frozen plan bytes changed')
    return _verify(root, load(plan_path), load(manifest_path), load(observations_path), digest(manifest_path))


def verify_archive(root, plan_path, manifest_path, observations_path):
    result = verify_committed(root, plan_path, manifest_path, observations_path)
    manifest, observations = collect(root, plan_path)
    require(manifest == load(manifest_path), 'Raw acquisition differs from committed manifest')
    require(observations == load(observations_path), 'Raw observations differ from committed evidence')
    return dict(result, raw_source_bytes_verified=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('plan', type=Path)
    parser.add_argument('manifest', type=Path)
    parser.add_argument('observations', type=Path)
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--committed-only', action='store_true')
    args = parser.parse_args()
    if args.verify_only or args.committed_only:
        verify = verify_committed if args.committed_only else verify_archive
        print(json.dumps(verify(args.root, args.plan, args.manifest, args.observations)))
        return
    require(not args.manifest.exists() and not args.observations.exists(), 'Archive output exists')
    manifest, observations = collect(args.root, args.plan)
    for path, value in ((args.manifest, manifest), (args.observations, observations)):
        with path.open('xb') as stream:
            stream.write(payload(value))
    print(json.dumps(verify_archive(args.root, args.plan, args.manifest, args.observations)))


if __name__ == '__main__':
    main()

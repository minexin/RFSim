"""Prepare ten independent, byte-audited order-study inputs; no COM calls."""

import argparse
import importlib.util
import json
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    'raw_axis_preparer', Path(__file__).with_name('prepare-am-pm-datapoints-raw-axis-study.py'))
previous = importlib.util.module_from_spec(spec)
spec.loader.exec_module(previous)
require, digest = previous.require, previous.digest
prepare_document, planned_fields = previous.prepare_document, previous.planned_fields
source_expression = previous.source_expression
SOURCE_PATH, SOURCE_SHA256 = previous.SOURCE_PATH, previous.SOURCE_SHA256
PLAN_SHA256 = '3fd61758408c87d1fb3e163ceee04a8ceb9b75f77a880ae4ea3a8c0ab2e58de6'
PHYSICS = dict(power_dbm=[-5, 5, 15], phase_deg=[10, 40, -20], gain_db=20,
               frequency_hz=100000000.0, source_phase_deg=37, output_p1db_dbm=100,
               output_saturation_dbm=103, output_ip2_dbm=140, output_ip3_dbm=130,
               sweep_input_start_dbm=-30, sweep_input_stop_dbm=0, sweep_step_db=0.5,
               expected_points=61)
PROFILE_ORDER = (
    ('a-zero', 0, 'historical_reproduction'),
    ('a-positive-epsilon', 0.000001, 'historical_reproduction'),
    ('a-positive-delta', 0.00001, 'new_prediction_check'),
    ('a-negative-delta', -0.00001, 'new_prediction_check'),
    ('a-negative-epsilon', -0.000001, 'historical_reproduction'),
    ('b-zero', 0, 'historical_reproduction'),
    ('b-negative-epsilon', -0.000001, 'historical_reproduction'),
    ('b-negative-delta', -0.00001, 'order_reproduction'),
    ('b-positive-delta', 0.00001, 'order_reproduction'),
    ('b-positive-epsilon', 0.000001, 'historical_reproduction'),
)
PROFILE_PAIRS = tuple((name, 'b-' + name[2:]) for name, _, _ in PROFILE_ORDER[:5])


def validate_plan(plan):
    """Check the frozen preparation contract independently of scorer readiness."""
    require(type(plan['schema_version']) is int and plan['schema_version'] == 1,
            'Unsupported order-study schema')
    require(plan['source_workspace_sha256'] == SOURCE_SHA256, 'Plan source hash changed')
    require(plan['analysis_budget'] == dict(profiles=10, reserved_setup_recovery=0, total_maximum=10),
            'Order-study analysis budget changed')
    profiles = plan['profiles']
    require(isinstance(profiles, list) and len(profiles) == 10, 'Exactly ten planned profiles required')
    expected_keys = set(PHYSICS) | {'name', 'ordinary_output_target_dbm', 'ordinary_role', 'block', 'block_position'}
    for index, (profile, (name, target, role)) in enumerate(zip(profiles, PROFILE_ORDER)):
        require(set(profile) == expected_keys, 'Order-study profile fields changed')
        require(profile['name'] == name and profile['ordinary_role'] == role,
                'Registered profile order or role changed')
        require(profile['block'] == name[0] and type(profile['block_position']) is int
                and profile['block_position'] == index % 5, 'Registered block position changed')
        require(previous.decimal(profile['ordinary_output_target_dbm']) == previous.decimal(target),
                'Registered ordinary target changed')
        for field, expected in PHYSICS.items():
            value = profile[field]
            if isinstance(expected, list):
                require(type(value) is list and len(value) == len(expected)
                        and list(map(previous.decimal, value)) == list(map(previous.decimal, expected)),
                        'Registered physical configuration changed: ' + field)
            else:
                require(previous.decimal(value) == previous.decimal(expected),
                        'Registered physical configuration changed: ' + field)
        require(type(profile['expected_points']) is int, 'Expected point count must be an integer')
    require(plan['order_comparisons']['pairs'] == [list(pair) for pair in PROFILE_PAIRS],
            'Registered same-target pair collection changed')
    primary = plan['primary_evaluation']
    require(primary['historical_reproduction_count'] == 6 and primary['new_prediction_check_count'] == 2
            and primary['order_reproduction_count'] == 2 and primary['paired_target_count'] == 5
            and primary['new_prediction_profile_names'] == ['a-positive-delta', 'a-negative-delta'],
            'Registered role accounting changed')
    return profiles


def prepare(root, plan_path, output_directory):
    root, plan_path, output_directory = (Path(path).resolve() for path in (root, plan_path, output_directory))
    require(output_directory.is_relative_to(root) and output_directory != root,
            'Output directory must remain inside root')
    require(not output_directory.exists(), 'Preparation output already exists')
    plan_bytes = plan_path.read_bytes()
    require(digest(plan_bytes) == PLAN_SHA256, 'Registered plan hash changed')
    plan = json.loads(plan_bytes)
    profiles = validate_plan(plan)
    source = (root / SOURCE_PATH).resolve()
    require(source.is_relative_to(root), 'Source workspace escaped root')
    raw = source.read_bytes()
    require(digest(raw) == SOURCE_SHA256, 'Source workspace hash changed')
    prepared = []
    for profile in profiles:
        data, changes = prepare_document(raw, profile)
        require(len(changes) == len({change['path'] for change in changes}) == 17
                and {change['path']: change['after'] for change in changes} == planned_fields(profile),
                'Prepared workspace must have exactly seventeen registered XML edits')
        workspace = output_directory / 'profiles' / profile['name'] / 'RFModel_AMPM_A01.wsv'
        prepared.append((profile, data, changes, workspace))
    require(len({workspace.parent for _, _, _, workspace in prepared}) == 10,
            'Each invocation requires an independent input directory')
    by_name = {profile['name']: (profile, data, changes) for profile, data, changes, _ in prepared}
    pairs = []
    for left, right in PROFILE_PAIRS:
        profile, left_bytes, left_changes = by_name[left]
        _, right_bytes, right_changes = by_name[right]
        require(left_bytes == right_bytes and left_changes == right_changes,
                'Same-target A/B prepared workspace bytes differ: ' + left)
        pairs.append(dict(profiles=[left, right], ordinary_output_target_dbm=profile['ordinary_output_target_dbm'],
                          workspace_sha256=digest(left_bytes), byte_identical=True))
    # All ten inverse semantic audits and five exact byte comparisons complete
    # before creation of any destination directory or workspace file.
    manifest = dict(schema_version=1, plan_sha256=digest(plan_bytes), source_path=SOURCE_PATH,
                    source_sha256=SOURCE_SHA256, independent_input_directories=10,
                    same_target_pairs=pairs, profiles=[])
    output_directory.mkdir(parents=True, exist_ok=False)
    for profile, data, changes, workspace in prepared:
        workspace.parent.mkdir(parents=True, exist_ok=False)
        with workspace.open('xb') as stream:
            stream.write(data)
        manifest['profiles'].append(dict(name=profile['name'], workspace_path=workspace.relative_to(root).as_posix(),
                                        workspace_sha256=digest(data), changes=changes,
                                        source_power_expression=source_expression(
                                            profile['ordinary_output_target_dbm'], profile['gain_db']),
                                        block=profile['block'], block_position=profile['block_position'],
                                        ordinary_role=profile['ordinary_role']))
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
    print('Prepared', len(result['profiles']), 'independent inputs with 17 audited XML edits each;'
          ' all five same-target A/B pairs have identical bytes.')


if __name__ == '__main__':
    main()

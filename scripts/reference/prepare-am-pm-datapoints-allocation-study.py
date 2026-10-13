"""Prepare twelve independent gain/source-allocation inputs; no COM calls."""

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
source_expression, gain_expression = previous.source_expression, previous.expression
SOURCE_PATH, SOURCE_SHA256 = previous.SOURCE_PATH, previous.SOURCE_SHA256
PLAN_SHA256 = 'df5cf362e9fc2fb852965c5a18bcf5c9bb556b3387f7b40246eb0bb04cf6a91c'
PHYSICS = dict(power_dbm=[-5, 5, 15], phase_deg=[10, 40, -20],
               frequency_hz=100000000.0, source_phase_deg=37, output_p1db_dbm=100,
               output_saturation_dbm=103, output_ip2_dbm=140, output_ip3_dbm=130,
               sweep_input_start_dbm=-30, sweep_input_stop_dbm=0, sweep_step_db=0.5,
               expected_points=61)
PROFILE_ORDER = (
    ('g20-start-zero', 20, 0),
    ('g20-negative-epsilon', 20, -0.000001),
    ('g20-positive-epsilon', 20, 0.000001),
    ('g18-zero', 18, 0),
    ('g18-negative-epsilon', 18, -0.000001),
    ('g18-positive-epsilon', 18, 0.000001),
    ('g18-fixed-input', 18, -2),
    ('g22-zero', 22, 0),
    ('g22-positive-epsilon', 22, 0.000001),
    ('g22-negative-epsilon', 22, -0.000001),
    ('g22-fixed-input', 22, 2),
    ('g20-end-zero', 20, 0),
)
PROFILE_PAIRS = (('g20-start-zero', 'g20-end-zero'),)
PROFILE_METADATA = ('ordinary_role', 'gain_group', 'comparison_role', 'control_profile')


def validate_plan(plan):
    """Freeze preparation independently of the scientific scorer."""
    require(type(plan['schema_version']) is int and plan['schema_version'] == 1,
            'Unsupported allocation-study schema')
    require(plan['source_workspace_sha256'] == SOURCE_SHA256, 'Plan source hash changed')
    budget = dict(profiles=12, reserved_setup_recovery=0, total_maximum=12)
    require(plan['analysis_budget'] == budget
            and all(type(plan['analysis_budget'][key]) is int for key in budget),
            'Allocation-study analysis budget changed')
    profiles = plan['profiles']
    require(isinstance(profiles, list) and len(profiles) == 12, 'Exactly twelve planned profiles required')
    expected_keys = set(PHYSICS) | {'name', 'gain_db', 'ordinary_output_target_dbm'} | set(PROFILE_METADATA)
    for profile, (name, gain, target) in zip(profiles, PROFILE_ORDER):
        require(set(profile) == expected_keys, 'Allocation-study profile fields changed')
        require(profile['name'] == name, 'Registered profile order changed')
        group = 'g' + str(gain)
        historical = gain == 20
        role = 'historical_reproduction' if historical else 'new_prediction_check'
        comparison = 'historical_control' if historical else (
            'same_nominal_input' if name.endswith('fixed-input') else 'same_nominal_output')
        control = 'g20-start-zero' if historical else group + '-zero'
        require(profile['gain_group'] == group and profile['ordinary_role'] == role
                and profile['comparison_role'] == comparison and profile['control_profile'] == control,
                'Registered gain group, role, or control changed')
        require(previous.decimal(profile['gain_db']) == previous.decimal(gain),
                'Registered gain changed')
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
    return profiles


def prepare(root, plan_path, output_directory):
    root, plan_path, output_directory = (Path(path).resolve() for path in (root, plan_path, output_directory))
    require(output_directory.is_relative_to(root) and output_directory != root,
            'Output directory must remain inside root')
    require(not output_directory.exists(), 'Preparation output already exists')
    plan_bytes = plan_path.read_bytes()
    require(digest(plan_bytes) == PLAN_SHA256, 'Registered plan hash changed')
    profiles = validate_plan(json.loads(plan_bytes))
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
    require(len({workspace.parent for _, _, _, workspace in prepared}) == 12,
            'Each invocation requires an independent input directory')
    by_name = {profile['name']: (profile, data, changes) for profile, data, changes, _ in prepared}
    pairs = []
    for left, right in PROFILE_PAIRS:
        profile, left_bytes, left_changes = by_name[left]
        _, right_bytes, right_changes = by_name[right]
        require(left_bytes == right_bytes and left_changes == right_changes,
                'Same-configuration prepared workspace bytes differ: ' + left)
        pairs.append(dict(profiles=[left, right], gain_db=profile['gain_db'],
                          ordinary_output_target_dbm=profile['ordinary_output_target_dbm'],
                          workspace_sha256=digest(left_bytes), byte_identical=True))
    # Distinct gain/source allocations must not accidentally reuse one input,
    # even when their nominal input or output powers are the same.
    for index, (left, left_bytes, _, _) in enumerate(prepared):
        for right, right_bytes, _, _ in prepared[index + 1:]:
            if (left['name'], right['name']) not in PROFILE_PAIRS:
                require(left_bytes != right_bytes, 'Different configurations reused identical workspace bytes')
    # Finish every inverse semantic audit and pair comparison before any write.
    manifest = dict(schema_version=1, plan_sha256=digest(plan_bytes), source_path=SOURCE_PATH,
                    source_sha256=SOURCE_SHA256, independent_input_directories=12,
                    same_configuration_pairs=pairs, profiles=[])
    output_directory.mkdir(parents=True, exist_ok=False)
    for profile, data, changes, workspace in prepared:
        workspace.parent.mkdir(parents=True, exist_ok=False)
        with workspace.open('xb') as stream:
            stream.write(data)
        manifest['profiles'].append(dict(name=profile['name'], workspace_path=workspace.relative_to(root).as_posix(),
                                        workspace_sha256=digest(data), changes=changes,
                                        gain_expression=gain_expression(profile['gain_db']),
                                        source_power_expression=source_expression(
                                            profile['ordinary_output_target_dbm'], profile['gain_db']),
                                        **{field: profile[field] for field in PROFILE_METADATA}))
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
          ' only the first and last G20 zero controls have identical bytes.')


if __name__ == '__main__':
    main()

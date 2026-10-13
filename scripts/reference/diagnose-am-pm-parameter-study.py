"""Score frozen AM/PM mappings through the developer C++ probe, with no fitting."""
import argparse
import importlib.util
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_replay import NativeReplay, atomic_write_report, forbid_product_imports
forbid_product_imports()
spec = importlib.util.spec_from_file_location('study_archive', Path(__file__).with_name('archive-am-pm-parameter-study.py'))
study = importlib.util.module_from_spec(spec)
spec.loader.exec_module(study)
require, number = study.require, study.number
archive = study.archive


def analyze(evidence, plan, native):
    require(evidence['schema_version'] == 1 and evidence['plan_sha256'] == study.PLAN_SHA, 'Wrong evidence plan')
    require(evidence['hypotheses'] == plan['hypotheses'], 'Frozen hypotheses changed')
    require(evidence['phase_absolute_tolerance_degrees'] == plan['phase_absolute_tolerance_degrees'] == 1e-5,
            'Registered tolerance changed')
    samples = evidence['samples']
    require(len(samples) == len(plan['profiles']) == 6, 'Six profiles required')
    rows = {'sweep': [], 'ordinary_report': [], 'ordinary_raw': []}
    for sample, profile in zip(samples, plan['profiles']):
        require(sample['profile'] == {k: v for k, v in profile.items() if k != 'predictions'}, 'Profile changed')
        start = archive.parse_utc(sample['run_started_utc'])
        end = archive.parse_utc(sample['run_returned_utc'])
        require(start <= end, 'Reversed run times')
        require(set(sample['timestamps']) == {study.PATH+'1', study.PATH+'2', study.RAW}, 'Wrong dataset identities')
        require(all(math.floor(start) <= number(t) <= math.ceil(end) for t in sample['timestamps'].values()), 'Stale dataset')
        expected_parameters = study.expected_parameters(profile)
        require(set(sample['parameters_si']) == set(expected_parameters), 'Missing parameter readbacks')
        for key, wanted in expected_parameters.items():
            require(abs(number(sample['parameters_si'][key])-wanted) <= 1e-12*max(1e-12, abs(wanted)), 'Parameter mismatch: '+key)
        require(len(sample['sweep']) == 73, 'Incomplete sweep')
        for i, point in enumerate(sample['sweep']):
            require(type(point['index']) is int and point['index'] == i, 'Changed sweep order')
            expected = profile['sweep_input_start_dbm'] + .25*i + 20
            require(point['nominal_output_dbm'] == expected, 'Changed nominal grid')
            power = number(point['input_power_w'])
            require(power > 0 and abs(10*math.log10(power)+30-(expected-20)) < 1e-5, 'Measured input grid mismatch')
        ordinary = sample['ordinary']
        for axis, points in [('sweep', sample['sweep']), ('ordinary_report', [ordinary]), ('ordinary_raw', [ordinary])]:
            for point in points:
                power = number(point['raw_output_w' if axis == 'ordinary_raw' else 'output_power_w'])
                require(power > 0, 'Positive measured power required')
                source = number(point['input_phase_rad'])
                require(abs(source-(0 if axis == 'sweep' else math.pi/6)) < 1e-10, 'Source ownership changed')
                actual = math.degrees(number(point['output_phase_rad'])-source)
                hypotheses = {}
                for hypothesis in plan['hypotheses']:
                    response = native.am_pm_response(
                        power, peak_degrees_per_db=profile['peak_degrees_per_db']*hypothesis['peak_scale'],
                        output_reference_dbm=profile['reference_dbm']+hypothesis['reference_shift_db'])
                    predicted = math.degrees(number(response.phase_rad))
                    error = archive.hypotheses.circular_error(actual, predicted)
                    hypotheses[hypothesis['name']] = dict(predicted_phase_deg=predicted,
                                                         circular_error_deg=error, passed=error <= 1e-5)
                rows[axis].append(dict(profile=profile['name'], index=point['index'], output_power_w=power,
                                       output_dbm=10*math.log10(power)+30, input_phase_rad=source,
                                       systemvue_increment_deg=actual, hypotheses=hypotheses))
    def summary(axis_rows):
        return {h['name']: dict(point_count=len(axis_rows),
                               passed_count=sum(r['hypotheses'][h['name']]['passed'] for r in axis_rows),
                               passed=all(r['hypotheses'][h['name']]['passed'] for r in axis_rows),
                               max_circular_error_deg=max(r['hypotheses'][h['name']]['circular_error_deg'] for r in axis_rows))
                for h in plan['hypotheses']}
    axes = {axis: dict(summary=summary(values), profiles={p['name']: summary([r for r in values if r['profile'] == p['name']])
                                                        for p in plan['profiles']}, rows=values) for axis, values in rows.items()}
    return dict(schema_version=1, classification='frozen_parameter_mapping_transfer_test_not_full_compatibility',
                new_systemvue_analysis_calls=0, phase_absolute_tolerance_degrees=1e-5,
                hypotheses=plan['hypotheses'], axes=axes, parameters_fitted_this_study=0,
                eligible_for_full_model_compatibility=False, product_formula_changed=False)


def verify_raw(evidence, plan, root):
    for sample, profile in zip(evidence['samples'], plan['profiles']):
        source = sample['capture_source']
        path = root / source['path']
        require(archive.digest(path) == source['sha256'], 'Raw bytes changed')
        actual = study.normalize(archive.load(path), profile)
        actual['capture_source'] = source
        require(actual == sample, 'Normalized observation differs from raw capture')
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('plan', type=Path)
    p.add_argument('evidence', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--native-probe', type=Path, required=True)
    p.add_argument('--raw-root', type=Path)
    args = p.parse_args()
    require(not args.output.exists(), 'Output exists')
    require(archive.digest(args.plan) == study.PLAN_SHA, 'Registered plan bytes changed')
    evidence, plan = archive.load(args.evidence), archive.load(args.plan)
    raw_verified = verify_raw(evidence, plan, args.raw_root) if args.raw_root else False
    native = NativeReplay(args.native_probe)
    result = analyze(evidence, plan, native)
    result.update(raw_sources_verified=raw_verified, plan_sha256=archive.digest(args.plan),
                  evidence_sha256=archive.digest(args.evidence),
                  native_provenance=native.manifest([args.plan, args.evidence],
                                                    [Path(__file__), Path(study.__file__), Path(archive.__file__),
                                                     Path(archive.hypotheses.__file__)]))
    atomic_write_report(args.output, result, protected_paths=[args.plan, args.evidence, args.native_probe, native.manifest_path])
    for axis, data in result['axes'].items():
        print(axis, data['summary'])
    return 0 if result['axes']['sweep']['summary']['native_analytic']['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())

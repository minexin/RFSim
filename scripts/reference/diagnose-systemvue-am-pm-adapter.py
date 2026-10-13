"""Replay the opt-in C++ AM/PM model without applying mappings in Python."""
import argparse
import importlib.util
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from native_replay import NativeReplay, atomic_write_report, forbid_product_imports
forbid_product_imports()


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


current = module('current_archive', 'archive-am-pm-adapter-study.py')
prior = module('prior_diagnostic', 'diagnose-am-pm-parameter-study.py')
archive, study = current.archive, current.study
require, number = study.require, study.number
ROOT = Path(__file__).resolve().parents[2]
OLD_PLAN = ROOT/'validation/am-pm-point-derivative-plan-20261013.json'
PLAN = ROOT/'validation/am-pm-point-derivative-adapter-plan-20261013.json'
AMENDMENT = ROOT/'validation/am-pm-point-derivative-adapter-amendment-20261013.json'


def validate_sample(sample, profile, new):
    require(sample['profile'] == {k: v for k, v in profile.items() if k != 'predictions'}, 'Profile changed')
    count = profile.get('expected_points', 73)
    require(len(sample['sweep']) == count, 'Registered points missing')
    extra = sample.get('additional_sweep_points', [])
    require(len(extra) == int(new), 'Extra endpoint retention changed')
    diagnostics = sample.get('diagnostics', '')
    flagged = new and profile['name'] == '10GHz-saturation'
    require(bool(diagnostics) == flagged, 'Warning label changed')
    if new:
        require(sample['warning_free_acceptance_eligible'] == (not flagged), 'Warning eligibility changed')
    if flagged:
        require('driven past the input 1 dB compression point' in diagnostics, 'Wrong diagnostic')
    start, end = map(archive.parse_utc, (sample['run_started_utc'], sample['run_returned_utc']))
    require(start <= end, 'Invalid run interval')
    require(set(sample['timestamps']) == {study.PATH+'1', study.PATH+'2', study.RAW}, 'Wrong dataset identities')
    require(all(math.floor(start) <= number(v) <= math.ceil(end) for v in sample['timestamps'].values()), 'Stale data')
    expected = study.expected_parameters(profile)
    require(set(sample['parameters_si']) == set(expected), 'Missing parameters')
    for key, wanted in expected.items():
        require(abs(number(sample['parameters_si'][key])-wanted) <= 1e-12*max(1e-12, abs(wanted)), 'Parameter mismatch')
    for i, point in enumerate(sample['sweep']+extra):
        nominal_input = profile['sweep_input_start_dbm']+profile.get('sweep_step_db', .25)*i
        require(type(point['index']) is int and point['index'] == i
                and point['nominal_output_dbm'] == nominal_input+profile.get('gain_db', 20),
                'Grid or ordinal changed')
        power = number(point['input_power_w'])
        require(power > 0 and abs(10*math.log10(power)+30-nominal_input) < 1e-5, 'Measured grid changed')
        require(abs(number(point['input_phase_rad'])) < 1e-10, 'Sweep source phase changed')
    require(abs(number(sample['ordinary']['input_phase_rad'])-math.radians(profile.get('source_phase_deg', 30))) < 1e-10,
            'Ordinary source phase changed')
    return flagged


def analyze(old, new, old_plan, amendment, native):
    require(old['plan_sha256'] == study.PLAN_SHA and new['plan_sha256'] == current.PLAN_SHA, 'Wrong evidence plan')
    require(old['phase_absolute_tolerance_degrees'] == new['phase_absolute_tolerance_degrees'] == 1e-5, 'Tolerance changed')
    require(old['hypotheses'] == new['hypotheses'] == old_plan['hypotheses'], 'Frozen hypothesis changed')
    require(len(old['samples']) == len(old_plan['profiles']) == 6, 'Old profiles missing')
    require(len(new['samples']) == len(amendment['effective_profiles']) == 4, 'New profiles missing')
    entries = [(s, p, False) for s, p in zip(old['samples'], old_plan['profiles'])]
    entries += [(s, p, True) for s, p in zip(new['samples'], amendment['effective_profiles'])]
    flags = [validate_sample(s, p, n) for s, p, n in entries]
    axes = {key: [] for key in ('registered_sweep', 'additional_sweep', 'ordinary_report', 'ordinary_raw')}
    for (sample, profile, is_new), flagged in zip(entries, flags):
        for axis, points in [('registered_sweep', sample['sweep']),
                             ('additional_sweep', sample.get('additional_sweep_points', [])),
                             ('ordinary_report', [sample['ordinary']]), ('ordinary_raw', [sample['ordinary']])]:
            for point in points:
                power = number(point['raw_output_w' if axis == 'ordinary_raw' else 'output_power_w'])
                require(power > 0, 'Positive power required')
                observed = math.degrees(number(point['output_phase_rad'])-number(point['input_phase_rad']))
                models = {}
                for name, method in [('legacy_analytic', native.am_pm_response),
                                     ('systemvue2023_calibrated', native.systemvue2023_am_pm_response)]:
                    value = method(power, peak_degrees_per_db=profile['peak_degrees_per_db'],
                                   output_reference_dbm=profile['reference_dbm'])
                    predicted = math.degrees(number(value.phase_rad))
                    error = archive.hypotheses.circular_error(observed, predicted)
                    models[name] = dict(phase_deg=predicted, radial_phase_slope=number(value.radial_phase_slope),
                                        circular_error_deg=error, numeric_passed=error <= 1e-5)
                axes[axis].append(dict(profile=profile['name'], new_holdout=is_new, warning_bearing=flagged,
                                       index=point['index'], output_power_w=power, observed_increment_deg=observed, models=models))

    def summarize(rows):
        return {name: dict(point_count=len(rows), numeric_passed_count=sum(r['models'][name]['numeric_passed'] for r in rows),
                           max_circular_error_deg=max((r['models'][name]['circular_error_deg'] for r in rows), default=0))
                for name in ('legacy_analytic', 'systemvue2023_calibrated')}
    return dict(schema_version=1, classification='opt_in_calibrated_cpp_model_numeric_replay',
                parameters_fitted=0, new_systemvue_analysis_calls=0, phase_absolute_tolerance_degrees=1e-5,
                eligible_for_full_model_compatibility=False,
                axes={axis: dict(all_points=summarize(rows),
                                 warning_free=summarize([r for r in rows if not r['warning_bearing']]),
                                 warning_diagnostic=summarize([r for r in rows if r['warning_bearing']]), rows=rows)
                      for axis, rows in axes.items()})


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('prior_evidence', type=Path)
    p.add_argument('evidence', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--native-probe', type=Path, required=True)
    p.add_argument('--raw-root', type=Path)
    args = p.parse_args()
    require(not args.output.exists(), 'Output exists')
    require(archive.digest(OLD_PLAN) == study.PLAN_SHA and archive.digest(PLAN) == current.PLAN_SHA, 'Plan bytes changed')
    old, new = map(archive.load, [args.prior_evidence, args.evidence])
    old_plan, amendment = map(archive.load, [OLD_PLAN, AMENDMENT])
    require(archive.digest(AMENDMENT) == new['amendment_sha256'], 'Amendment bytes changed')
    manifest_path = ROOT/'validation/am-pm-adapter-captures-20261013.json'
    require(archive.digest(manifest_path) == new['manifest_sha256'], 'Capture manifest bytes changed')
    if args.raw_root:
        prior.verify_raw(old, old_plan, args.raw_root)
        for sample, profile in zip(new['samples'], amendment['effective_profiles']):
            require(current.normalized_source(args.raw_root, sample['capture_source'], profile) == sample,
                    'Raw normalization changed')
    native = NativeReplay(args.native_probe)
    result = analyze(old, new, old_plan, amendment, native)
    result['raw_sources_verified'] = bool(args.raw_root)
    result['native_provenance'] = native.manifest(
        [args.prior_evidence, args.evidence, OLD_PLAN, PLAN, AMENDMENT, manifest_path],
        [Path(__file__), Path(current.__file__), Path(prior.__file__), Path(study.__file__),
         Path(archive.__file__), Path(archive.hypotheses.__file__)])
    atomic_write_report(args.output, result, protected_paths=[args.prior_evidence, args.evidence, args.native_probe,
                                                             native.manifest_path])
    print(result['axes']['registered_sweep']['all_points'])
    scores = [axis['warning_free']['systemvue2023_calibrated'] for axis in result['axes'].values()]
    return 0 if all(s['numeric_passed_count'] == s['point_count'] for s in scores) else 1


if __name__ == '__main__':
    sys.exit(main())

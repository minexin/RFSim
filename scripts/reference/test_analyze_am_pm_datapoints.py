"""Independent numerical and evidence-boundary checks; no COM calls."""

import copy
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path(__file__).with_name("analyze-am-pm-datapoints.py")
spec = importlib.util.spec_from_file_location("ampm_diagnostic", SCRIPT)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
PLAN = ROOT / "validation/am-pm-datapoints-plan-20261012.json"


class DataPointsHypothesisTests(unittest.TestCase):
    def setUp(self):
        self.plan = json.loads(PLAN.read_text(encoding="utf-8-sig"))

    def predict(self, x, phases=(10, 40, -20), powers=(0, 10, 20), **kwargs):
        options = dict(
            axis="dbm", phase_rule="numeric", boundary="constant", composition="add_source"
        )
        options.update(kwargs)
        return m.predict(powers, phases, x, 0, **options)

    def samples(self):
        # Independent hand-calculated dBm-linear golden points, not predict().
        phases = {
            "interpolation-boundary": [10, 10, 25, 39.997, 40, 39.994, 10, -20, -20],
            "wrap": [170, 0, -170],
            "unwrapped-equivalent": [170, 180, 190],
            "full-turn": [10, 190, 370],
            "source-phase-offset": [50],
        }
        result = []
        for profile in self.plan["profiles"]:
            for x, phase in zip(profile["output_targets_dbm"], phases[profile["name"]]):
                result.append(
                    dict(
                        profile=profile["name"],
                        target_output_dbm=x,
                        measured_output_w=10 ** ((x - 30) / 10),
                        measured_output_phase_deg=phase,
                        measured_source_phase_deg=profile["source_phase_deg"],
                    )
                )
        return result

    def test_distinct_axis_predictions(self):
        self.assertEqual(self.predict(5), 25)
        self.assertAlmostEqual(self.predict(5, axis="watt"), 10 + 30 * (math.sqrt(10) - 1) / 9)
        self.assertAlmostEqual(
            self.predict(5, axis="amplitude"), 10 + 30 * (10**0.25 - 1) / (math.sqrt(10) - 1)
        )
        self.assertEqual(self.predict(15), 10)

    def test_phase_turns_are_retained_until_measurement_comparison(self):
        self.assertEqual(self.predict(5, [170, -170], [0, 10]), 0)
        self.assertEqual(self.predict(5, [170, -170], [0, 10], phase_rule="shortest"), 180)
        self.assertEqual(self.predict(5, [10, 370], [0, 10]), 190)
        self.assertAlmostEqual(self.predict(5, [10, 370], [0, 10], phase_rule="phasor"), 10)
        self.assertEqual(m.circular_error(190, -170), 0)
        self.assertEqual(m.circular_error(10, 370), 0)

    def test_phasor_and_shortest_can_differ(self):
        value = self.predict(2.5, [0, 90], [0, 10], phase_rule="phasor")
        self.assertAlmostEqual(value, math.degrees(math.atan(1 / 3)))
        self.assertEqual(self.predict(2.5, [0, 90], [0, 10], phase_rule="shortest"), 22.5)
        with self.assertRaises(ValueError):
            self.predict(5, [0, 180], [0, 10], phase_rule="phasor")
        with self.assertRaises(ValueError):
            self.predict(5, [0, 180], [0, 10], phase_rule="shortest")

    def test_boundaries_and_knots(self):
        self.assertEqual([self.predict(x) for x in [-5, 0, 10, 20, 25]], [10, 10, 40, -20, -20])
        self.assertEqual(self.predict(-5, boundary="linear"), -5)
        self.assertEqual(self.predict(25, boundary="linear"), -50)
        self.assertAlmostEqual(self.predict(9.999), 39.997)
        self.assertAlmostEqual(self.predict(10.001), 39.994)

    def test_all_golden_points_distinguish_candidate_and_never_promote_compatibility(self):
        result = m.analyze(self.plan, {"samples": self.samples()})
        matches = [r["hypothesis"] for r in result["candidates"] if r["matched_all_observed"]]
        self.assertEqual(
            matches,
            [dict(axis="dbm", phase_rule="numeric", boundary="constant", composition="add_source")],
        )
        self.assertEqual(result["observed_sample_count"], 19)
        self.assertTrue(result["all_planned_samples_present"])
        for key in [
            "eligible_for_vendor_compatibility",
            "baseline_verified",
            "observation_provenance_verified",
        ]:
            self.assertFalse(result[key])

    def test_empty_and_partial_evidence_do_not_silently_pass(self):
        result = m.analyze(self.plan)
        self.assertFalse(result["all_planned_samples_present"])
        self.assertFalse(any(r["matched_all_observed"] for r in result["candidates"]))
        result = m.analyze(self.plan, {"samples": self.samples()[:1]})
        self.assertFalse(result["all_planned_samples_present"])
        self.assertGreater(sum(r["matched_all_observed"] for r in result["candidates"]), 1)

    def test_actual_power_is_used_instead_of_target(self):
        sample = self.samples()[2]
        sample["measured_output_w"] = 10 ** ((6 - 30) / 10)
        sample["measured_output_phase_deg"] = 28
        result = m.analyze(self.plan, {"samples": [sample]})
        candidate = next(
            r
            for r in result["candidates"]
            if r["hypothesis"]
            == dict(axis="dbm", phase_rule="numeric", boundary="constant", composition="add_source")
        )
        self.assertTrue(candidate["matched_all_observed"])
        self.assertAlmostEqual(candidate["residuals"][0]["actual_output_dbm"], 6)

    def test_invalid_table_or_numeric_input(self):
        for powers, phases in [
            ([0], [10]),
            ([0, 0], [10, 20]),
            ([10, 0], [10, 20]),
            ([0, 10], [10]),
            ([0, math.nan], [10, 20]),
            ([0, True], [10, 20]),
        ]:
            with self.subTest(powers=powers), self.assertRaises(ValueError):
                self.predict(1, phases, powers)
        for value in [math.nan, math.inf, True, "1"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.predict(value)
        with self.assertRaises(ValueError):
            self.predict(5, axis="unsupported")

    def test_wrong_or_duplicate_observations(self):
        sample = self.samples()[0]
        for key, value in [
            ("profile", "unknown"),
            ("target_output_dbm", 123),
            ("measured_output_w", 0),
            ("measured_output_w", -1),
            ("measured_source_phase_deg", 1),
            ("measured_output_phase_deg", math.nan),
        ]:
            row = dict(sample, **{key: value})
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                m.analyze(self.plan, {"samples": [row]})
        with self.assertRaises(ValueError):
            m.analyze(self.plan, {"samples": [sample, sample]})
        plan = copy.deepcopy(self.plan)
        plan["profiles"].append(plan["profiles"][0])
        with self.assertRaises(ValueError):
            m.analyze(plan)

    def test_natural_spline_posthoc_candidate_and_independent_followup_values(self):
        self.assertEqual(m.natural_cubic([0, 10, 20], [10, 40, -20], 5), 33.4375)
        self.assertEqual(m.natural_cubic([0, 10, 20], [10, 40, -20], 15), 18.4375)
        self.assertEqual(m.natural_cubic([0, 10, 20], [10, 40, -20], 2.5), 22.7734375)
        self.assertAlmostEqual(m.natural_cubic([0, 10, 20], [10, 40, -20], 0.001), 10.0052499999775)
        self.assertEqual(m.natural_cubic([0, 10], [10, 370], 5), 190)
        self.assertEqual(m.natural_cubic([0, 10, 20], [10, 40, -20], -5), 10)
        self.assertEqual(m.natural_cubic([0, 10, 20], [10, 40, -20], 25), -20)
        # Unequal intervals with a globally linear function must stay linear.
        self.assertAlmostEqual(m.natural_cubic([0, 2, 7, 10], [1, 5, 15, 21], 4), 9)

    def test_zero_dbm_exception_is_retained_as_failure(self):
        sample = self.samples()[1]
        sample["measured_output_phase_deg"] = 0
        result = m.assess_natural_cubic(self.plan, {"samples": [sample]})
        self.assertFalse(result["all_observed_matched"])
        self.assertEqual(result["rows"][0]["circular_error_deg"], 10)
        self.assertFalse(result["eligible_for_vendor_compatibility"])

    def test_reported_and_raw_axes_are_separate(self):
        sample = self.samples()[2]
        sample["measured_output_w"] = 0.001
        sample["raw_output_w"] = 10 ** ((5 - 30) / 10)
        sample["measured_output_phase_deg"] = 33.4375
        observations = {"samples": [sample]}
        reported = m.assess_natural_cubic(self.plan, observations)
        raw = m.assess_natural_cubic(self.plan, observations, "raw_output_w")
        self.assertFalse(reported["all_observed_matched"])
        self.assertTrue(raw["all_observed_matched"])
        self.assertNotEqual(reported["power_axis_field"], raw["power_axis_field"])
        self.assertFalse(raw["eligible_for_vendor_compatibility"])

    def test_cli_does_not_overwrite_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / "result.json"
            args = [sys.executable, str(SCRIPT), str(PLAN), str(out)]
            first = subprocess.run(args, capture_output=True, timeout=10)
            self.assertEqual(first.returncode, 0, first.stderr)
            saved = out.read_bytes()
            second = subprocess.run(args, capture_output=True, timeout=10)
            self.assertNotEqual(second.returncode, 0)
            self.assertEqual(out.read_bytes(), saved)


class ArchiveIntegrityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = SCRIPT.with_name("archive-am-pm-datapoints.py")
        spec = importlib.util.spec_from_file_location("archive_ampm", path)
        cls.archive = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.archive)
        cls.observations = json.loads(
            (ROOT / "validation/am-pm-datapoints-observations-20261012.json").read_text()
        )

    def setUp(self):
        # Minimal synthetic capture populated from the committed measured scalar/phasor evidence.
        a = self.archive
        self.sample = self.observations["samples"][2]
        self.profile = json.loads(PLAN.read_text())["profiles"][0]
        s = self.sample
        nodes = []

        def add(path, value, stamp=None):
            nodes.append(
                dict(
                    path=path,
                    data=value,
                    dimensions=[len(value)] if isinstance(value, list) else None,
                    timestamp=stamp,
                )
            )

        for path, stamp in s["timestamps"].items():
            add(path, None, str(stamp))
        key = a.PATH + "/Eqns/VarBlock/"
        add(key + "NodeNames", ["1", "2"])
        add(key + "CF", [1e8, 1e8])
        add(key + "DCP", [s["source_parameter_power_w"], s["measured_output_w"]])
        add(
            key + "DCPH",
            [
                math.radians(s["measured_source_phase_deg"]),
                math.radians(s["measured_output_phase_deg"]),
            ],
        )
        for name, value in s["parameter_readbacks"].items():
            add(a.AMP + name, value)
        add(a.SOURCE + "Pwr", s["source_parameter_power_w"])
        add(a.SOURCE + "Phase", math.radians(s["measured_source_phase_deg"]))
        key = a.RAW + "/Eqns/VarBlock/"
        add(key + "IDName", [s["raw_carrier"][0]["label"]])
        add(key + "IDNo", [7])
        add(key + "ID2", [7, 7])
        add(key + "F2", [r["frequency_hz"] for r in s["raw_carrier"]])
        add(key + "P2", [r["power_w"] for r in s["raw_carrier"]])
        add(key + "V2", [v for r in s["raw_carrier"] for v in r["voltage"]])
        add(key + "Z2", [v for r in s["raw_carrier"] for v in r["impedance"]])
        self.capture = dict(
            nodes=nodes,
            manager_errors="",
            target_output_dbm=s["target_output_dbm"],
            run_started_utc=s["run_started_utc"],
            run_returned_utc=s["run_returned_utc"],
        )

    def node(self, path):
        return next(n for n in self.capture["nodes"] if n["path"] == path)

    def test_windows_timestamp_fraction_and_explicit_utc(self):
        self.assertAlmostEqual(self.archive.parse_utc("1970-01-01T00:00:00.1234567Z"), 0.123456)
        self.assertEqual(self.archive.parse_utc("1970-01-01T00:00:00+00:00"), 0)
        for text in ["1970-01-01T00:00:00", "1970-01-01T00:00:00+08:00", "bad"]:
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.archive.parse_utc(text)

    def test_actual_phasor_fixture_has_consistent_units_and_phase(self):
        result = self.archive.normalize(self.capture, self.profile)
        self.assertAlmostEqual(result["measured_output_phase_deg"], 33.43749960741896)
        self.assertAlmostEqual(result["raw_output_w"], self.sample["raw_output_w"])

    def test_stale_ambiguous_and_wrong_unit_evidence_rejected(self):
        original = copy.deepcopy(self.capture)
        self.node(self.archive.PATH)["timestamp"] = "0"
        with self.assertRaises(ValueError):
            self.archive.normalize(self.capture, self.profile)
        self.capture = copy.deepcopy(original)
        self.capture["nodes"].append(copy.deepcopy(self.node(self.archive.PATH)))
        with self.assertRaises(ValueError):
            self.archive.normalize(self.capture, self.profile)
        self.capture = copy.deepcopy(original)
        self.node(self.archive.AMP + "PwrAMtoPMpoints")["data"] = [0, 10, 20]
        with self.assertRaises(ValueError):
            self.archive.normalize(self.capture, self.profile)

    def test_warning_and_wrong_frequency_rejected(self):
        self.capture["manager_errors"] = "warning"
        with self.assertRaises(ValueError):
            self.archive.normalize(self.capture, self.profile)
        self.capture["manager_errors"] = ""
        self.node(self.archive.PATH + "/Eqns/VarBlock/CF")["data"][1] = 2e8
        with self.assertRaises(ValueError):
            self.archive.normalize(self.capture, self.profile)

    def test_voltage_power_tampering_rejected(self):
        self.node(self.archive.RAW + "/Eqns/VarBlock/V2")["data"][0] *= 2
        with self.assertRaises(ValueError):
            self.archive.normalize(self.capture, self.profile)

    def test_report_disagreement_remains_visible(self):
        self.node(self.archive.PATH + "/Eqns/VarBlock/DCP")["data"][1] = 0.001
        result = self.archive.normalize(self.capture, self.profile)
        self.assertFalse(result["report_vs_raw_power_match_at_1e_minus7"])
        self.assertEqual(result["measured_output_w"], 0.001)
        self.assertNotEqual(result["raw_output_w"], 0.001)

    def test_recovered_capture_requires_completion_evidence(self):
        end = self.capture["run_returned_utc"]
        self.capture["run_returned_utc"] = None
        with self.assertRaises(ValueError):
            self.archive.normalize(self.capture, self.profile)
        self.capture["analysis_done"] = 1
        self.capture["analysis_done_observed_utc"] = end
        result = self.archive.normalize(self.capture, self.profile)
        self.assertIsNone(result["run_returned_utc"])
        self.assertEqual(result["analysis_done_observed_utc"], end)

    def test_committed_counterexamples_are_not_reclassified(self):
        r = self.observations
        self.assertFalse(r["eligible_for_vendor_compatibility"])
        self.assertFalse(r["spline_report_axis"]["all_observed_matched"])
        self.assertFalse(r["spline_raw_axis"]["all_observed_matched"])
        zeros = [s for s in r["samples"] if s["target_output_dbm"] == 0]
        self.assertEqual(len(zeros), 4)
        self.assertTrue(all(s["measured_output_phase_deg"] == 0 for s in zeros))
        near_zero = next(s for s in r["samples"] if s["target_output_dbm"] == 0.001)
        self.assertFalse(near_zero["report_vs_raw_power_match_at_1e_minus7"])
        self.assertGreater(abs(near_zero["measured_output_phase_deg"] - 10), 0.005)


if __name__ == "__main__":
    unittest.main()

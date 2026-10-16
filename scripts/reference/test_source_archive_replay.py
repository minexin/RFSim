"""Immutable 23-capture source replay and full-origin negative controls.

Run with python -I -S test_source_archive_replay.py --native-probe PROBE.
Passing the gate retains the four observed shared-drive vendor failures.
Protocol/independent C++ oracle tests live in test_source_replay_protocol.py.
"""
import argparse
import copy
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

parser = argparse.ArgumentParser(add_help=False)
parser.add_argument("--native-probe", type=Path, required=True)
ARGS, remaining = parser.parse_known_args()
PROBE = ARGS.native_probe.resolve()
sys.argv[1:] = remaining
ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))
from native_source_replay import NativeSourceReplay
from native_replay import NativeReplayError, atomic_write_report
from reference_records import CoherentComponent, SourceCoherence, SpectrumKind
import replay_source_archives as facade


def load_script(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), SCRIPTS / name)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


shared = load_script("compare-shared-compression.py")
coherent = load_script("compare-coherent-amplifier.py")


def captures():
    return json.loads((ROOT / "validation" / facade.CAPTURES).read_bytes())


def map_case(capture):
    recorder = facade._RecordingProvider(NativeSourceReplay(PROBE))
    report = coherent.compare(recorder, [capture])
    call = recorder.coherent_calls[0]
    mapping = facade._source_mapping(capture, call, shared, call["result"])
    return report, mapping, call["result"]


class FullArchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original = captures()
        cls.report = facade.replay(PROBE)

    def test_all_two_scopes_are_separate_and_preserve_53_and_206_checks(self):
        r, summary = self.report, self.report["summary"]
        self.assertEqual(summary["capture_count"], 23)
        self.assertEqual(summary["scored_case_count"], 21)
        self.assertEqual(summary["excluded_diagnostic_count"], 2)
        self.assertEqual(summary["direct_boundary_complex_comparison_count"], 64)
        self.assertEqual(summary["group_partition_check_count"], 21)
        self.assertEqual(summary["group_partition_passed_count"], 21)
        self.assertEqual((summary["shared"]["check_count"], summary["shared"]["passed_count"],
                          summary["shared"]["failed_count"]), (53, 49, 4))
        self.assertFalse(summary["shared"]["vendor_passed"])
        self.assertEqual((summary["coherent"]["check_count"], summary["coherent"]["passed_count"],
                          summary["coherent"]["failed_count"]), (206, 206, 0))
        self.assertTrue(summary["coherent"]["vendor_passed"])
        self.assertTrue(r["reports"]["shared"]["direct_carrier_passed"])
        self.assertFalse(r["reports"]["shared"]["drive_power_passed"])
        self.assertFalse(r["reports"]["shared"]["passed"])
        self.assertEqual(r["new_systemvue_analysis_calls"], 0)
        self.assertFalse(r["eligible_for_full_model_compatibility"])
        for kind, count in (("shared", 53), ("coherent", 206)):
            rows = r["comparison"][kind]["rows"]
            self.assertEqual(len(rows), count)
            self.assertEqual(len({(row["case_id"], facade._json_key(row["identity"])) for row in rows}), count)
            self.assertFalse(r["comparison"][kind]["status_differences"])
            for row in rows:
                self.assertIn("current", row)
                self.assertIn("historical", row)
                self.assertIn("delta", row)
                if kind == "shared" and row["identity"]["measurement"] == "direct_carrier":
                    self.assertTrue(row["identity"]["identity"].startswith("{"))
                    self.assertTrue(row["identity"]["identity"].endswith("}" + row["identity"]["origin"]))
        self.assertFalse(any(p["delta"]["status_changed"] for p in r["group_partitions"]))

    def test_exact_four_failures_and_independent_no_noise_controls(self):
        rows = self.report["comparison"]["shared"]["rows"]
        failed = [r for r in rows if not r["current"]["passed"]]
        self.assertEqual([r["capture_index"] for r in failed], [7, 9, 12, 16])
        self.assertTrue(all(r["identity"] == {"measurement": "RFPwrIn(RFAmp)"} for r in failed))
        by_index = {r["capture_index"]: r["current"] for r in rows if r["identity"]["measurement"] == "RFPwrIn(RFAmp)"}
        for noisy, quiet in ((7, 18), (9, 19), (12, 20), (16, 22)):
            a, b = by_index[noisy], by_index[quiet]
            self.assertTrue(b["passed"])
            self.assertEqual(a["rfmodel_w"], b["rfmodel_w"])
            self.assertAlmostEqual(a["systemvue_w"] - b["systemvue_w"], 1.6015544319643966e-11, delta=1e-18)
            self.assertEqual(a["tolerance_w"], max(1e-15, a["systemvue_w"] * 1e-7))

    def test_missing_output_and_noise_only_output_are_distinct_exclusions(self):
        excluded = self.report["diagnostic_exclusions"]
        self.assertEqual([r["case_id"] for r in excluded], ["shared-21", "shared-23"])
        self.assertEqual([r["observation_state"] for r in excluded],
            ["all_five_output_vectors_absent", "spectrum_present_without_deterministic_carriers"])
        for row in excluded:
            self.assertFalse(row["measured_zero"])
            self.assertFalse(row["affects_compatibility_verdict"])
        diagnostics = self.report["reports"]["shared"]["diagnostics"]
        self.assertFalse(diagnostics[0]["output_spectrum_present"])
        self.assertTrue(diagnostics[1]["output_spectrum_present"])
        self.assertGreater(diagnostics[1]["systemvue_rfpwrin_w"], 1e-11)
        self.assertLess(diagnostics[0]["systemvue_rfpwrin_w"], 1e-30)
        self.assertTrue(all(d["direct_carriers"] == 0 for d in diagnostics))
        self.assertFalse(any(row["capture_index"] in (21, 23)
            for kind in ("shared", "coherent") for row in self.report["comparison"][kind]["rows"]))

    def test_alias_equivalence_members_and_signed_factor_multiplicity(self):
        mappings = self.report["source_equivalence_mapping"]
        self.assertEqual(len(mappings), 23)
        alias = mappings[14]
        self.assertEqual(alias["capture_index"], 15)
        self.assertEqual(alias["definitions"][0]["nominal_power_dbm"], -26)
        self.assertEqual(alias["definitions"][1]["nominal_power_dbm"], -6)
        self.assertEqual(len(alias["equivalence_classes"]), 1)
        group = alias["equivalence_classes"][0]
        self.assertEqual(group["contributing_source_ids"], ["Source1", "Source2"])
        self.assertEqual(group["observed_representative"], "Source2")
        self.assertEqual(group["reduced_input_index"], 1)
        waves = [complex(*d["native_input"]["amplitude_sqrt_w"]) for d in alias["definitions"]]
        self.assertNotEqual(waves[0], 0)
        self.assertNotEqual(waves[1], 0)
        self.assertAlmostEqual(abs(complex(*group["native_reduced_input"]["amplitude_sqrt_w"]) - sum(waves)), 0, delta=1e-17)
        self.assertTrue(all("Source.Source2" in o["identity"] for o in alias["origins"]))
        carrier_cubic = next(o for o in alias["origins"] if o["input_indices"] == [-1, 1, 1])
        self.assertEqual([f["sign"] for f in carrier_cubic["signed_source_equivalence_factors"]], [-1, 1, 1])
        self.assertEqual(len(carrier_cubic["signed_source_equivalence_factors"]), 3)
        self.assertEqual(carrier_cubic["kind"], "INTERMOD")
        self.assertEqual(carrier_cubic["bandwidth_hz"], 3)
        same_bin = [o for o in alias["origins"] if o["bin"] == 10]
        self.assertEqual(len(same_bin), 2)
        self.assertNotEqual(same_bin[0]["identity"], same_bin[1]["identity"])
        self.assertEqual(len(mappings[2]["equivalence_classes"]), 2)  # Independent 180 degrees do not cancel.
        self.assertEqual(len(mappings[5]["equivalence_classes"]), 1)  # Locked quadrature adds phasors.

    def test_provenance_has_actual_bytes_and_only_unverified_historical_claims(self):
        identity = self.report["archive_identity"]
        raw = (ROOT / "validation" / facade.CAPTURES).read_bytes()
        self.assertEqual(identity["available_capture_bytes_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(identity["reconstructed_crlf_sha256"], hashlib.sha256(raw.replace(b"\n", b"\r\n")).hexdigest())
        self.assertNotEqual(identity["available_capture_bytes_sha256"], identity["historical_capture_sha256"])
        self.assertTrue(identity["crlf_relation_verified"])
        self.assertEqual(len(identity["raw_capture_claims"]), 23)
        self.assertEqual(len(identity["workspace_claims"]), 1)
        self.assertEqual(identity["workspace_claims"][0]["scope"], "on_disk_source_copy_at_archive_time")
        self.assertEqual(len(identity["historical_library_claims"]), 2)
        for field in ("raw_capture_claims", "workspace_claims", "historical_library_claims"):
            self.assertTrue(all(row["verification"] == "historical_declared_unverified" for row in identity[field]))
        for field in ("original_raw_verified", "original_workspace_verified", "original_library_verified"):
            self.assertFalse(identity[field])
        encoded = json.dumps(self.report)
        self.assertIn(hashlib.sha256(PROBE.read_bytes()).hexdigest(), encoded)
        for name, digest in facade.ARCHIVE_SHA256.items():
            self.assertEqual(hashlib.sha256((ROOT / "validation" / name).read_bytes()).hexdigest(), digest)
            self.assertIn(digest, encoded)
        self.assertIn("canonical_request", encoded)
        self.assertIn("response_sha256", encoded)

    def test_historical_comparison_joins_full_identity_not_row_order(self):
        for kind in ("shared", "coherent"):
            now = copy.deepcopy(self.report["reports"][kind])
            old = self.report["historical_reports"][kind]
            now["reports"].reverse()
            for case in now["reports"]:
                case["checks"].reverse()
            joined = facade._comparison(kind, now, old, self.original)
            self.assertEqual(len(joined["rows"]), 53 if kind == "shared" else 206)
            self.assertFalse(joined["status_differences"])

    def test_changes_are_reported_instead_of_forcing_historical_numbers(self):
        current = copy.deepcopy(self.report["reports"]["coherent"])
        check = current["reports"][0]["checks"][0]
        check["rfmodel_amplitude"][0] *= 1.25
        check["rfmodel_power_w"] *= 1.25 ** 2
        check["relative_complex_error"] = .25
        check["relative_power_error"] = .5625
        check["passed"] = False
        result = facade._comparison("coherent", current, self.report["historical_reports"]["coherent"], self.original)
        self.assertEqual(len(result["status_differences"]), 1)
        changed = result["status_differences"][0]
        self.assertFalse(changed["current"]["passed"])
        self.assertTrue(changed["historical"]["passed"])
        self.assertIn("rfmodel_amplitude", changed["delta"]["numeric"])
        self.assertEqual(changed["identity"]["identity"], check["identity"])

    def test_identity_measurement_and_coverage_drift_rejected(self):
        for change in ("origin", "index", "kind", "width", "bin", "observation", "missing", "duplicate", "configuration"):
            now = copy.deepcopy(self.report["reports"]["coherent"])
            case, check = now["reports"][0], now["reports"][0]["checks"][0]
            if change == "origin": check["identity"] += ",unexpected"
            elif change == "index": check["input_indices"] = [2]
            elif change == "kind": check["kind"] = "INTERMOD"
            elif change == "width": check["bandwidth_hz"] = 3.
            elif change == "bin": check["bin"] = 11
            elif change == "observation": check["systemvue_power_w"] *= 1.01
            elif change == "missing": case["checks"].pop()
            elif change == "duplicate": case["checks"][1] = copy.deepcopy(check)
            else: case["source_phases_deg"] = [0, 45]
            with self.subTest(change=change), self.assertRaises(ValueError):
                facade._comparison("coherent", now, self.report["historical_reports"]["coherent"], self.original)
        for change in ([], self.original[:-1], self.original[::-1], self.original[:22] + self.original[:1]):
            with self.assertRaises(ValueError):
                facade._validate_cases(change)


class OriginAndCaptureMutationTests(unittest.TestCase):
    def setUp(self):
        self.captures = captures()
        self.provider = NativeSourceReplay(PROBE)

    def test_wrong_alias_fails_full_origin_lookup_with_numerics_unchanged(self):
        capture = self.captures[14]
        original = copy.deepcopy(capture)
        report, mapping, prediction = map_case(capture)
        before_prediction = copy.deepcopy(prediction)
        original_origins = facade._validate_source_mapping(capture, mapping, prediction, coherent)
        self.assertTrue(report["passed"])
        self.assertEqual(len(original_origins), 4)
        mapping["equivalence_classes"][0]["observed_representative"] = "Source1"
        with self.assertRaisesRegex(ValueError, "Missing/ambiguous direct product"):
            facade._validate_source_mapping(capture, mapping, prediction, coherent)
        self.assertEqual(prediction, before_prediction)
        self.assertEqual(capture, original)
        # This fault is an alias identity error, not a changed numerical model.
        self.assertTrue(all(c["passed"] for c in report["reports"][0]["checks"]))

    def test_mapped_key_and_membership_faults_rejected_without_numeric_changes(self):
        capture = self.captures[14]
        _, mapping, prediction = map_case(capture)
        original_prediction = copy.deepcopy(prediction)
        for fault in ("key", "membership", "definition", "amplitude"):
            changed = copy.deepcopy(mapping)
            entry = changed["equivalence_classes"][0]
            if fault == "key": entry["key"]["native_group"] += 1
            elif fault == "membership": entry["contributing_source_ids"] = ["Source2"]
            elif fault == "definition": changed["definitions"].pop(0)
            else: entry["native_reduced_input"]["amplitude_sqrt_w"][0] *= 2
            with self.subTest(fault=fault), self.assertRaises(ValueError):
                facade._validate_source_mapping(capture, changed, prediction, coherent)
            self.assertEqual(prediction, original_prediction)

    def test_wrong_alias_mapper_cannot_publish_a_complete_replay(self):
        original = facade._source_mapping
        def wrong_alias(capture, *args, **kwargs):
            result = original(capture, *args, **kwargs)
            if capture["source_powers_dbm"] == [-26., -6.] and capture["locked"]:
                result["equivalence_classes"][0]["observed_representative"] = "Source1"
            return result
        with mock.patch.object(facade, "_source_mapping", side_effect=wrong_alias):
            with self.assertRaisesRegex(ValueError, "Missing/ambiguous direct product"):
                facade.replay(PROBE)

    def test_removing_weaker_source_is_numerically_detected_separately(self):
        capture = self.captures[14]
        original = copy.deepcopy(capture)
        cubic, compression = self.provider.coherent_amplifier, self.provider.compress_coherent_fundamentals
        def drop_weak_cubic(spacing, components, **options):
            self.assertEqual(len(components), 2)
            self.assertLess(abs(components[0].amplitude), abs(components[1].amplitude))
            return cubic(spacing, components[1:], **options)
        def drop_weak_shared(spacing, components, **options):
            return compression(spacing, components[1:], **options)
        with mock.patch.object(self.provider, "coherent_amplifier", side_effect=drop_weak_cubic):
            result = coherent.compare(self.provider, [capture])
        self.assertFalse(result["passed"])
        self.assertEqual(len(result["reports"][0]["checks"]), 4)
        self.assertTrue(all(not c["passed"] for c in result["reports"][0]["checks"]))
        with mock.patch.object(self.provider, "compress_coherent_fundamentals", side_effect=drop_weak_shared):
            result = shared.compare(self.provider, [capture])
        self.assertFalse(result["direct_carrier_passed"])
        self.assertFalse(result["drive_power_passed"])
        self.assertEqual(capture, original)

    def test_missing_duplicate_extra_kind_bandwidth_and_origin_faults_rejected(self):
        original = self.provider.coherent_amplifier
        for mutation in ("missing", "duplicate", "extra", "kind", "width", "wrong-origin"):
            def corrupt(*args, **kwargs):
                result = original(*args, **kwargs)
                if mutation == "missing": return result._replace(terms=result.terms[:-1])
                if mutation == "duplicate": return result._replace(terms=result.terms + result.terms[:1])
                term = result.terms[-1]
                if mutation == "kind": term = term._replace(component=term.component._replace(kind=SpectrumKind.SOURCE))
                elif mutation == "width": term = term._replace(component=term.component._replace(bandwidth_hz=4.))
                elif mutation == "extra":
                    term = term._replace(component=term.component._replace(bin=34))
                    return result._replace(terms=result.terms + (term,))
                else: term = term._replace(input_indices=(1, 1, 2))
                return result._replace(terms=result.terms[:-1] + (term,))
            with self.subTest(mutation=mutation), mock.patch.object(self.provider, "coherent_amplifier", side_effect=corrupt):
                with self.assertRaises(ValueError): coherent.compare(self.provider, self.captures[:1])

    def test_phase_zero_and_group_partition_faults_fail_verdict(self):
        original = self.provider.coherent_amplifier
        for mutation in ("phase", "zero", "group"):
            def corrupt(*args, **kwargs):
                result = original(*args, **kwargs)
                term = result.terms[-1]
                component = (term.component._replace(coherence_group=result.terms[-2].component.coherence_group)
                    if mutation == "group" else term.component._replace(amplitude=-term.component.amplitude
                                                                         if mutation == "phase" else 0j))
                return result._replace(terms=result.terms[:-1] + (term._replace(component=component),))
            with self.subTest(mutation=mutation), mock.patch.object(self.provider, "coherent_amplifier", side_effect=corrupt):
                report = coherent.compare(self.provider, self.captures[:1])
                self.assertFalse(report["passed"])
                if mutation == "group":
                    self.assertFalse(report["reports"][0]["group_partition_passed"])
                    self.assertTrue(all(c["passed"] for c in report["reports"][0]["checks"]))
                else: self.assertFalse(report["reports"][0]["checks"][-1]["passed"])

    def test_joint_power_condition_catches_error_below_complex_threshold(self):
        capture = self.captures[0]
        baseline = coherent.compare(self.provider, [capture])["reports"][0]["checks"][-1]
        target = complex(*baseline["systemvue_amplitude"]) * (1 + 7e-8)
        original = self.provider.coherent_amplifier
        def power_only(*args, **kwargs):
            result = original(*args, **kwargs)
            term = result.terms[-1]
            return result._replace(terms=result.terms[:-1] + (term._replace(component=term.component._replace(amplitude=target)),))
        with mock.patch.object(self.provider, "coherent_amplifier", side_effect=power_only):
            report = coherent.compare(self.provider, [capture])
        check = report["reports"][0]["checks"][-1]
        self.assertLess(check["relative_complex_error"], 1e-7)
        self.assertGreater(abs(check["relative_power_error"]), 1e-7)
        self.assertFalse(check["passed"])
        self.assertFalse(report["passed"])

    def test_numeric_groups_are_local_labels_not_source_identifiers(self):
        original = self.provider.coherent_amplifier
        def relabel(*args, **kwargs):
            result = original(*args, **kwargs)
            return result._replace(terms=tuple(t._replace(component=t.component._replace(
                coherence_group=t.component.coherence_group + 400)) for t in result.terms))
        with mock.patch.object(self.provider, "coherent_amplifier", side_effect=relabel):
            report = coherent.compare(self.provider, self.captures[:1])
        self.assertTrue(report["passed"])

    def test_metadata_readbacks_warnings_shapes_and_finite_values_rejected(self):
        for mutation in ("noise", "clock", "frequency", "phase", "power", "version", "script", "warning",
                         "stale-main", "stale-path", "totals", "topology", "gain", "shape", "bool", "nan", "inf"):
            capture = copy.deepcopy(self.captures[0])
            if mutation == "noise": capture["noise_enabled"] = False
            elif mutation == "clock": capture["locked"] = True
            elif mutation == "frequency": capture["same_frequency"] = True
            elif mutation == "phase": capture["source_phases_deg"] = [0., 45.]
            elif mutation == "power": capture["source_powers_dbm"] = [-6., -26.]
            elif mutation == "version": capture["systemvue_version"] = "2024"
            elif mutation == "script": capture.pop("script_language")
            elif mutation == "warning": capture["manager_errors"] = "(WARNING) unrelated failure"
            elif mutation.startswith("stale"):
                suffix = "System1_Data" if mutation == "stale-main" else "System1_Data_Folder/System1_Data_Path1"
                shared.two.node(capture, suffix)["timestamp"] = "0"
            elif mutation == "totals": capture["analysis_settings"]["ShowTotals"] = 0
            elif mutation == "topology": shared.two.node(capture, "Sch1/PartList/RFAmp")["path"] += "2"
            elif mutation == "gain": shared.two.node(capture, "Sch1/PartList/RFAmp/ParamSet/G")["data"] *= 2
            elif mutation == "shape": shared.two.node(capture, shared.two.SPECTRUM + "V2")["dimensions"] = [1]
            else:
                identifier = next(i for i, name in zip(shared.two.vector(capture, "IDNo"),
                    shared.two.vector(capture, "IDName")) if "D[(Source.Source1)],Source,RFAmp" in name)
                row = shared.two.vector(capture, "ID2").index(identifier)
                shared.two.node(capture, shared.two.SPECTRUM + "P2")["data"][row] = {
                    "bool": True, "nan": math.nan, "inf": math.inf}[mutation]
            with self.subTest(mutation=mutation):
                for checker in (shared, coherent):
                    with self.assertRaises((ValueError, KeyError)): checker.compare(self.provider, [capture])

    def test_observed_voltage_power_impedance_and_boundary_faults_rejected(self):
        for mutation in ("voltage", "power", "impedance", "boundary", "missing-voltage", "duplicate-id", "extra-origin"):
            capture = copy.deepcopy(self.captures[0])
            if mutation == "missing-voltage":
                capture["nodes"] = [n for n in capture["nodes"] if not n["path"].endswith("/V2")]
            elif mutation == "duplicate-id":
                shared.two.vector(capture, "IDNo")[1] = shared.two.vector(capture, "IDNo")[0]
            elif mutation == "extra-origin":
                names = shared.two.vector(capture, "IDName")
                ids = shared.two.vector(capture, "IDNo")
                active = set(shared.two.vector(capture, "ID2"))
                position = next(i for i, (identifier, name) in enumerate(zip(ids, names))
                                if identifier in active and "Node Noise" in name)
                names[position] = "{999}[(Source.Source1)+4x(Source.Source2)],RFAmp"
            else:
                field = {"voltage": "V2", "power": "P2", "impedance": "Z2", "boundary": "F2"}[mutation]
                values = shared.two.vector(capture, field)
                # Choose an actual deterministic RF row, not a noise/total sample.
                identity = next(i for i, n in zip(shared.two.vector(capture, "IDNo"),
                    shared.two.vector(capture, "IDName")) if "D[(Source.Source1)],Source,RFAmp" in n)
                row = shared.two.vector(capture, "ID2").index(identity)
                values[row * 2 if field in ("V2", "Z2") else row] *= 1.01
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                coherent.compare(self.provider, [capture])

    def test_diagnostics_never_excuse_partial_missing_ordinary_or_unknown_warning(self):
        for index, remove in ((20, {"F2"}), (22, {"F2"}), (0, {"F2", "P2", "ID2", "V2", "Z2"})):
            capture = copy.deepcopy(self.captures[index])
            if index == 20:
                # The allowed absence is all five. Supplying just one is malformed.
                source = copy.deepcopy(shared.two.node(self.captures[22], shared.two.SPECTRUM + "F2"))
                capture["nodes"].append(source)
            else:
                capture["nodes"] = [n for n in capture["nodes"] if n["path"].split("/")[-1] not in remove]
            with self.subTest(index=index), self.assertRaises(ValueError): shared.inspect(capture)
        for message in ("", "(WARNING) Unrelated failure"):
            capture = copy.deepcopy(self.captures[20])
            capture["manager_errors"] = message
            with self.assertRaises(ValueError): shared.inspect(capture)
        for values in ([], self.captures[:1] * 2, [c for c in self.captures if c["diagnostic"]]):
            for checker in (shared, coherent):
                with self.assertRaises(ValueError): checker.compare(self.provider, values)


class CliAndIsolationTests(unittest.TestCase):
    def run_cli(self, filename, *arguments, cwd=None):
        driver = ("import pathlib,runpy,sys; "
                  "sys.path.insert(0,str(pathlib.Path(sys.argv[1]).resolve().parent)); "
                  "sys.argv=sys.argv[1:]; runpy.run_path(sys.argv[0],run_name='__main__')")
        return subprocess.run([sys.executable, "-I", "-S", "-c", driver,
            str(SCRIPTS / filename), *(str(a) for a in arguments)], cwd=cwd,
            capture_output=True, text=True, timeout=90, check=False)

    def test_new_cli_explicit_opt_in_immutable_reports_and_all_byte_hashes(self):
        before = {name: (ROOT / "validation" / name).read_bytes() for name in facade.ARCHIVE_SHA256}
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "replay.json"
            missing = self.run_cli("replay_source_archives.py", "--report", output)
            self.assertNotEqual(missing.returncode, 0)
            self.assertFalse(output.exists())
            result = self.run_cli("replay_source_archives.py", "--native-probe", PROBE, "--report", output)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            saved = output.read_bytes()
            report = json.loads(saved)
            self.assertEqual(report["schema"], "rfmodel.source-archives-replay")
            self.assertFalse(report["summary"]["shared"]["vendor_passed"])
            self.assertTrue(report["summary"]["coherent"]["vendor_passed"])
            repeated = self.run_cli("replay_source_archives.py", "--native-probe", PROBE, "--report", output)
            self.assertNotEqual(repeated.returncode, 0)
            self.assertEqual(output.read_bytes(), saved)
            for protected in (PROBE, Path(str(PROBE) + ".manifest.json"),
                              *(ROOT / "validation" / name for name in facade.ARCHIVE_SHA256)):
                original = protected.read_bytes()
                result = self.run_cli("replay_source_archives.py", "--native-probe", PROBE, "--report", protected)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(protected.read_bytes(), original)
        self.assertEqual({name: (ROOT / "validation" / name).read_bytes() for name in before}, before)

    def test_three_retired_checker_clis_do_not_revive_library_or_native_switch(self):
        with self.assertRaisesRegex(RuntimeError, "retired"):
            shared.Library(PROBE)
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "forbidden.json"
            for script, capture in (("compare-shared-compression.py", facade.CAPTURES),
                                    ("compare-coherent-amplifier.py", facade.CAPTURES),
                                    ("compare-cascade-amplifier.py", "systemvue-2023-cascade-captures.json")):
                with self.subTest(script=script):
                    result = self.run_cli(script, PROBE, ROOT / "validation" / capture, output)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("retired", result.stderr.lower())
                    self.assertFalse(output.exists())
                    result = self.run_cli(script, PROBE, ROOT / "validation" / capture, output, "--native-probe", PROBE)
                    self.assertEqual(result.returncode, 2)
                    self.assertIn("unrecognized arguments: --native-probe", result.stderr)
                    self.assertFalse(output.exists())

    def test_frozen_checkers_records_and_runner_unchanged(self):
        for name, digest in facade.FROZEN_SHA256.items():
            with self.subTest(name=name): self.assertEqual(hashlib.sha256((SCRIPTS / name).read_bytes()).hexdigest(), digest)

    def test_cold_prestart_mutations_of_entire_frozen_dependency_closure_rejected(self):
        # A run-time snapshot alone would bless a scorer edited before startup.
        # Exercise the shipped entry in cold copied trees, without monkeypatching
        # the guard, and require rejection before any report can be published.
        with tempfile.TemporaryDirectory(prefix="rfmodel-source-frozen-") as tmp:
            root = Path(tmp)
            scripts = root / "scripts/reference"
            scripts.mkdir(parents=True)
            (root / "validation").mkdir()
            for source in SCRIPTS.glob("*.py"):
                shutil.copyfile(source, scripts / source.name)
            for name in facade.ARCHIVE_SHA256:
                shutil.copyfile(ROOT / "validation" / name, root / "validation" / name)
            for name in facade.FROZEN_SHA256:
                path = scripts / name
                original = path.read_bytes()
                path.write_bytes(original + b"\n# pre-start frozen dependency mutation\n")
                output = root / (name + ".json")
                with self.subTest(dependency=name):
                    process = subprocess.run([sys.executable, "-I", "-S", str(scripts / "replay_source_archives.py"),
                        "--native-probe", str(PROBE), "--report", str(output)], cwd=root,
                        capture_output=True, text=True, timeout=90, check=False)
                    self.assertNotEqual(process.returncode, 0)
                    self.assertIn("Frozen source checker/support bytes changed", process.stderr)
                    self.assertFalse(output.exists())
                path.write_bytes(original)
                # Some interpreters may otherwise reuse a same-size, same-second
                # pyc across successive copied-tree mutation processes.
                shutil.rmtree(scripts / "__pycache__", ignore_errors=True)

    def test_archive_mutation_and_post_run_snapshot_mismatch_cannot_publish(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "validation").mkdir()
            for name in facade.ARCHIVE_SHA256:
                shutil.copyfile(ROOT / "validation" / name, root / "validation" / name)
            path = root / "validation" / facade.CAPTURES
            path.write_bytes(path.read_bytes() + b" ")
            with self.assertRaisesRegex(ValueError, "Frozen source archive bytes"):
                facade.replay(PROBE, root=root)
        original = NativeSourceReplay.manifest
        def changed(provider, *args, **kwargs):
            result = original(provider, *args, **kwargs)
            hashes = result["available_archive_bytes_sha256"]
            hashes[next(iter(hashes))] = "f" * 64
            return result
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "changed.json"
            args = ["replay_source_archives.py", "--native-probe", str(PROBE), "--report", str(output)]
            with mock.patch.object(sys, "argv", args), mock.patch.object(NativeSourceReplay, "manifest", changed):
                with self.assertRaises(NativeReplayError): facade.main()
            self.assertFalse(output.exists())

    def test_atomic_write_rejects_existing_alias_symlink_and_nonfinite(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            original = root / "archive.json"
            original.write_bytes(b"preserved archive\n")
            with self.assertRaises((ValueError, FileExistsError)):
                atomic_write_report(root / ".." / root.name / original.name, {}, protected_paths=[original])
            output = root / "new.json"
            atomic_write_report(output, {"vendor_passed": False})
            with self.assertRaises(FileExistsError): atomic_write_report(output, {})
            with self.assertRaises(ValueError): atomic_write_report(root / "nan.json", {"bad": math.nan})
            if os.name != "nt":
                alias = root / "alias.json"
                alias.symlink_to(original)
                with self.assertRaises((ValueError, FileExistsError)): atomic_write_report(alias, {})
            self.assertEqual(original.read_bytes(), b"preserved archive\n")
            self.assertFalse((root / "nan.json").exists())
            self.assertFalse(list(root.glob(".native-replay-*")))

    def test_cold_isolated_replay_blocks_product_ctypes_and_library_discovery(self):
        driver = r'''
import importlib.abc
import pathlib
import runpy
import sys
class RejectProductAndBindings(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "rfmodel" or fullname.startswith("rfmodel.") or fullname in ("ctypes", "ctypes.util", "_ctypes"):
            raise AssertionError("forbidden product/binding/library discovery import: " + fullname)
        return None
root, probe = map(pathlib.Path, sys.argv[1:3])
assert not (root / "python").exists()
sys.path.insert(0, str(root / "scripts/reference"))
sys.meta_path.insert(0, RejectProductAndBindings())
script = root / "scripts/reference/replay_source_archives.py"
sys.argv = [str(script), "--native-probe", str(probe), "--report", str(root / "fresh.json")]
try:
    runpy.run_path(str(script), run_name="__main__")
except SystemExit as exc:
    if exc.code not in (0, None):
        raise
assert not any(n == "rfmodel" or n.startswith("rfmodel.") or n in ("ctypes", "ctypes.util", "_ctypes") for n in sys.modules)
print("ACTIVE_PRODUCT_BINDING_IMPORT_BAN_PASSED")
'''
        with tempfile.TemporaryDirectory(prefix="rfmodel-source-no-product-") as tmp:
            root = Path(tmp)
            scripts = root / "scripts/reference"
            scripts.mkdir(parents=True)
            (root / "validation").mkdir()
            for source in SCRIPTS.glob("*.py"):
                shutil.copyfile(source, scripts / source.name)
            for name in facade.ARCHIVE_SHA256:
                shutil.copyfile(ROOT / "validation" / name, root / "validation" / name)
            probe = root / PROBE.name
            shutil.copy2(PROBE, probe)
            shutil.copyfile(Path(str(PROBE) + ".manifest.json"), Path(str(probe) + ".manifest.json"))
            result = subprocess.run([sys.executable, "-I", "-S", "-c", driver, str(root), str(probe)],
                cwd=root, capture_output=True, text=True, timeout=90, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("ACTIVE_PRODUCT_BINDING_IMPORT_BAN_PASSED", result.stdout)
            self.assertTrue((root / "fresh.json").is_file())


if __name__ == "__main__":
    unittest.main()

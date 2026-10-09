"""Regress controlled spectrum reduction without hiding omissions or promoting fits."""

import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest

LIBRARY = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[2]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


diagnostic = load("reduction", "diagnose-spectrum-reduction.py")
archive = load("reduction_archive", "archive-cascade-reference.py")
comparison = diagnostic.comparison


class ReductionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = comparison.traced.two_tone.single.Library(LIBRARY)
        cls.references = json.loads(
            (ROOT / "validation/systemvue-2023-highorder-captures.json").read_text()
        )
        cls.captures = json.loads(
            (ROOT / "validation/systemvue-2023-spectrum-reduction-captures.json").read_text()
        )

    def report(self, captures=None):
        return diagnostic.diagnose(
            self.library, self.references, self.captures if captures is None else captures
        )

    def test_real_pairs_restore_all_origins_with_unchanged_retained_waves(self):
        report = self.report()
        self.assertEqual(report["classification"], "diagnostic_only")
        self.assertFalse(report["eligible_for_compatibility"])
        self.assertTrue(report["control_hypothesis_consistent"])
        self.assertEqual(sum(c["observed_count"] for c in report["cases"]), 224)
        for c in report["cases"]:
            self.assertEqual(c["predicted_count"], 44)
            self.assertTrue(c["labels_match_control_hypothesis"])
            self.assertTrue(c["fourth_order"]["consistent"])
            self.assertTrue(c["fifth_order"]["consistent"])
            if c["configuration"]["spectrum_reduction"]:
                self.assertFalse(c["full_group_sum_hypothesis"]["consistent"])
                self.assertEqual(c["full_group_sum_hypothesis"]["matching_count"], 0)
            else:
                self.assertEqual(c["not_recorded"], [])
        self.assertEqual(sorted(p["restored_count"] for p in report["pairs"]), [12, 14, 14])
        self.assertEqual(sum(p["common_count"] for p in report["pairs"]), 92)
        for p in report["pairs"]:
            self.assertTrue(p["full_expansion_recorded_when_disabled"])
            self.assertTrue(p["retained_waves_unchanged"])

    def test_missing_duplicate_and_uncontrolled_experiments_rejected(self):
        for captures in ([], self.captures[:-1], self.captures + self.captures[:1]):
            with self.subTest(count=len(captures)), self.assertRaises(ValueError):
                self.report(captures)
        for field in ("second_source_power_dbm", "spectrum_reduction"):
            captures = copy.deepcopy(self.captures)
            del captures[0][field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.report(captures)

    def test_reduction_and_threshold_readbacks_are_required(self):
        for suffix, bad in (("UseSpecReduction", 0), ("ElimSpec", 1), ("IgnorePwrLvl", 1e-18)):
            captures = copy.deepcopy(self.captures)
            c = next(c for c in captures if c["spectrum_reduction"])
            comparison.node(c, "System3/" + suffix)["data"] = bad
            with (
                self.subTest(suffix=suffix),
                self.assertRaisesRegex(ValueError, "Parameter mismatch"),
            ):
                self.report(captures)
        captures = copy.deepcopy(self.captures)
        captures[0]["spectrum_reduction"] = 1
        with self.assertRaisesRegex(ValueError, "boolean"):
            self.report(captures)

    def test_missing_restored_origin_fails_complete_coverage(self):
        captures = copy.deepcopy(self.captures)
        c = next(
            c
            for c in captures
            if not c["spectrum_reduction"] and c["source_power_dbm"] == c["second_source_power_dbm"]
        )
        label = "[-(Source.Source1)+3x(Source.Source1)],RFAmp1"
        number = next(
            i
            for i, name in zip(comparison.vector(c, "IDNo"), comparison.vector(c, "IDName"))
            if name.startswith("{") and name.endswith(label)
        )
        ids = comparison.vector(c, "ID3")
        keep = [i for i, identifier in enumerate(ids) if identifier != number]
        self.assertEqual(len(keep), len(ids) - 2)
        for name in ("F3", "P3", "ID3", "V3", "Z3"):
            n = comparison.node(c, comparison.SPECTRUM + name)
            width = 2 if name[0] in ("V", "Z") else 1
            n["data"] = [n["data"][width * i + j] for i in keep for j in range(width)]
            n["dimensions"] = [len(n["data"])]
        report = self.report(captures)
        self.assertFalse(report["control_hypothesis_consistent"])
        pair = next(p for p in report["pairs"] if p["source_powers_dbm"] == [-10, -10])
        self.assertFalse(pair["full_expansion_recorded_when_disabled"])

    def test_phase_change_is_not_hidden_by_power_or_label_checks(self):
        captures = copy.deepcopy(self.captures)
        c = next(c for c in captures if c["spectrum_reduction"])
        label = "[4x(Source.Source1)],RFAmp1"
        number = next(
            i
            for i, name in zip(comparison.vector(c, "IDNo"), comparison.vector(c, "IDName"))
            if name.startswith("{") and name.endswith(label)
        )
        volts = comparison.vector(c, "V3")
        for i, identifier in enumerate(comparison.vector(c, "ID3")):
            if identifier == number:
                volts[2 * i], volts[2 * i + 1] = -volts[2 * i + 1], volts[2 * i]
        report = self.report(captures)
        self.assertFalse(report["control_hypothesis_consistent"])
        self.assertFalse(all(p["retained_waves_unchanged"] for p in report["pairs"]))

    def test_archive_requires_command_and_new_metadata_agreement(self):
        c = next(
            c
            for c in self.captures
            if not c["spectrum_reduction"] and c["source_power_dbm"] != c["second_source_power_dbm"]
        )
        command = [
            "powershell.exe",
            "-WorkspacePath",
            "D:/project/RFModel/build-reference/RFModel_CascadeIntermods.wsv",
            "-TwoTone",
            "-DisableSpectrumReduction",
        ]
        for flag, key in (
            ("-SourcePowerDbm", "source_power_dbm"),
            ("-SourcePhaseDeg", "source_phase_deg"),
            ("-MaximumOrder", "maximum_order"),
            ("-SecondGainDb", "second_gain_db"),
            ("-ChannelBandwidthHz", "channel_bandwidth_hz"),
            ("-SecondaryRangeDb", "secondary_range_db"),
            ("-ReverseIsolationDb", "reverse_isolation_db"),
            ("-SecondSourcePowerDbm", "second_source_power_dbm"),
        ):
            command.extend([flag, str(c[key])])
        status = dict(
            case="cascade",
            state="captured",
            collector_exit_code=0,
            eligible_for_compatibility=True,
            command=command,
        )
        archived = archive.extract(c, status, c["raw_capture_sha256"])
        self.assertFalse(archived["spectrum_reduction"])
        comparison.node(archived, "System3/UseSpecReduction")
        mutations = [
            command + ["-DisableSpectrumReduction"],
            [x for x in command if x != "-DisableSpectrumReduction"],
            command[:-2],
            command[:-1] + ["-15"],
            command + ["-SecondSourcePowerDbm", "-20"],
        ]
        for changed in mutations:
            with self.subTest(command=changed), self.assertRaises(ValueError):
                archive.extract(c, dict(status, command=changed), c["raw_capture_sha256"])

    def test_legacy_comparisons_reject_new_modes(self):
        for source in (self.captures[1], self.captures[2]):
            c = copy.deepcopy(source)
            c["maximum_order"] = 3
            comparison.node(c, "System3/MaxOrder")["data"] = 3
            with self.assertRaisesRegex(
                ValueError, "requires (equal source powers|spectrum reduction enabled)"
            ):
                comparison.compare(self.library, [c])
        for source in (self.captures[1], self.captures[2]):
            captures = copy.deepcopy(self.references)
            captures[-1] = copy.deepcopy(source)
            with self.assertRaisesRegex(ValueError, "Uncontrolled high-order"):
                diagnostic.highorder.diagnose(self.library, captures)


if __name__ == "__main__":
    unittest.main()

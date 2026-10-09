"""Check measured-input diagnostics without hiding nominal-source discrepancies."""

import copy
import importlib.util
import json
import math
from pathlib import Path
import sys
import unittest

LIBRARY = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[2]


def load_module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


diagnostic = load_module("measured_input", "diagnose-highorder-input-reference.py")
archive = load_module("measured_input_archive", "archive-cascade-input-reference.py")


class InputReferenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = diagnostic.comparison.traced.two_tone.single.Library(LIBRARY)
        cls.captures = json.loads(
            (ROOT / "validation/systemvue-2023-eleventh-order-captures.json").read_text()
        )
        cls.references = json.loads(
            (ROOT / "validation/systemvue-2023-highorder-input-references.json").read_text()
        )

    def report(self, references=None):
        return diagnostic.diagnose(
            self.library, self.captures, self.references if references is None else references
        )

    def test_actual_input_explains_even_residuals_with_same_tolerance(self):
        report = self.report()
        self.assertFalse(report["eligible_for_compatibility"])
        self.assertEqual(report["relative_wave_tolerance"], 1e-7)
        self.assertEqual(report["classification"], "diagnostic_only")
        baseline = report["nominal_source_baseline"]["even_rules"]
        self.assertFalse(baseline["10"]["consistent"])
        self.assertGreater(baseline["10"]["max_relative_wave_error"], 1e-7)
        for family in ("even_rules", "odd_hypotheses"):
            self.assertTrue(all(s["consistent"] for s in report[family].values()))
            self.assertLess(
                max(s["max_relative_wave_error"] for s in report[family].values()), 1e-10
            )
        self.assertEqual(
            sum(
                s["observed_count"]
                for family in ("even_rules", "odd_hypotheses")
                for s in report[family].values()
            ),
            1413,
        )
        self.assertEqual(sum(len(c["not_recorded"]) for c in report["cases"]), 10)

    def test_fixed_reference_drive_differs_from_delivered_power(self):
        report = self.report()
        case = next(
            c
            for c in report["cases"]
            if c["configuration"]["two_tone"] and c["configuration"]["second_source_power_dbm"] == 5
        )
        observed = case["input_reference"]
        self.assertLess(observed["fixed_reference_drive_w"], observed["delivered_drive_w"])
        self.assertEqual(
            observed["fixed_reference_drive_w"],
            case["operating_point"]["fixed_reference_input_power_w"],
        )
        self.assertAlmostEqual(
            observed["delivered_drive_w"] / observed["native_input_power_w"], 1, places=12
        )
        for source in observed["sources"]:
            self.assertAlmostEqual(source["relative_to_nominal"][0], 0.9999999875, places=13)
            self.assertAlmostEqual(source["relative_to_nominal"][1], 0, places=13)

    def test_exact_capture_identity_is_required(self):
        for references in ([], self.references[:-1], self.references + self.references[:1]):
            with self.assertRaises(ValueError):
                self.report(references)
        references = copy.deepcopy(self.references)
        references[0]["raw_capture_sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            self.report(references)

    def test_inconsistent_samples_fail_before_scoring(self):
        for field, value in (
            ("frequency_hz", 123),
            ("power_w", 1),
            ("peak_voltage", [1, 0]),
            ("impedance_ohms", [50, 1]),
            ("peak_voltage", [math.nan, 0]),
        ):
            references = copy.deepcopy(self.references)
            references[0]["sources"][0]["samples"][0][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.report(references)

    def test_wrong_reference_port_and_source_identity_are_rejected(self):
        for key, value in (("reference_ohms", 75), ("source_port", 3)):
            references = copy.deepcopy(self.references)
            references[0][key] = value
            with self.assertRaises(ValueError):
                self.report(references)
        references = copy.deepcopy(self.references)
        references[0]["sources"][0]["label"] = "unrelated"
        with self.assertRaises(ValueError):
            self.report(references)

    def test_phase_corruption_cannot_be_absorbed_by_calibration(self):
        references = copy.deepcopy(self.references)
        capture = next(c for c in self.captures if c["source_phase_deg"] == 45)
        reference = next(
            r for r in references if r["raw_capture_sha256"] == capture["raw_capture_sha256"]
        )
        for sample in reference["sources"][0]["samples"]:
            value = complex(*sample["peak_voltage"]) * complex(0, 1)
            sample["peak_voltage"] = [value.real, value.imag]
        report = self.report(references)
        self.assertFalse(report["odd_hypotheses"]["11"]["consistent"])
        self.assertFalse(report["even_rules"]["6"]["consistent"])
        self.assertTrue(report["nominal_source_baseline"]["odd_hypotheses"]["11"]["consistent"])

    def test_archive_extracts_only_source_rows_and_rejects_truncation(self):
        raw = copy.deepcopy(self.captures[0])
        reference = next(
            r for r in self.references if r["raw_capture_sha256"] == raw["raw_capture_sha256"]
        )
        comparison = diagnostic.comparison
        names = dict(zip(comparison.vector(raw, "IDNo"), comparison.vector(raw, "IDName")))
        identifier = next(
            n
            for n, name in names.items()
            if name.startswith("{") and name.endswith(reference["sources"][0]["label"])
        )
        rows = reference["sources"][0]["samples"]
        vectors = {
            "F1": [r["frequency_hz"] for r in rows],
            "P1": [r["power_w"] for r in rows],
            "ID1": [identifier, identifier],
            "V1": [value for r in rows for value in r["peak_voltage"]],
            "Z1": [value for r in rows for value in r["impedance_ohms"]],
        }
        for name, values in vectors.items():
            raw["nodes"].append(
                {
                    "path": comparison.ROOT + comparison.SPECTRUM + name,
                    "data": values,
                    "dimensions": [len(values)],
                }
            )
        command = [
            "powershell.exe",
            "-WorkspacePath",
            "D:/project/RFModel/build-reference/RFModel_CascadeIntermods.wsv",
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
        ):
            command.extend([flag, str(raw[key])])
        status = dict(
            case="cascade",
            state="captured",
            collector_exit_code=0,
            eligible_for_compatibility=True,
            command=command,
        )
        result = archive.extract(raw, status, raw["raw_capture_sha256"])
        self.assertEqual(result, reference)
        comparison.node(raw, comparison.SPECTRUM + "V1")["data"] = None
        with self.assertRaises(ValueError):
            archive.extract(raw, status, raw["raw_capture_sha256"])


if __name__ == "__main__":
    unittest.main()

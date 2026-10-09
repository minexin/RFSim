"""Replay cascade observations, including failures; guard against false passes."""

import copy
import importlib.util
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

LIBRARY = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[2]


def load_module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


comparison = load_module("cascade_comparison", "compare-cascade-amplifier.py")
archive = load_module("cascade_archive", "archive-cascade-reference.py")


class CascadeReferenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = comparison.traced.two_tone.single.Library(LIBRARY)
        cls.original = json.loads(
            (ROOT / "validation/systemvue-2023-cascade-captures.json").read_text()
        )

    def setUp(self):
        self.captures = copy.deepcopy(self.original)

    def test_fifth_order_evidence_is_not_cubic_acceptance(self):
        captures = json.loads(
            (ROOT / "validation/systemvue-2023-secondary-controls-captures.json").read_text()
        )
        audit_module = load_module("secondary_audit", "audit-cascade-secondary-controls.py")
        report = audit_module.audit(captures)
        self.assertFalse(report["eligible_for_compatibility"])
        self.assertTrue(report["controls_unchanged"])
        self.assertEqual(
            {item["rf_origins"] for item in report["observations"] if item["port"] == 2}, {30, 94}
        )
        for capture in captures:
            if capture["maximum_order"] == 5:
                comparison.inspect(capture)
                with self.assertRaisesRegex(ValueError, "maximum_order=3"):
                    comparison.compare(self.library, [capture])
        with self.assertRaises(ValueError):
            audit_module.audit(captures[:-1])
        changed = copy.deepcopy(captures)
        changed[0]["two_tone"] = False
        with self.assertRaises(ValueError):
            audit_module.audit(changed)

    def test_actual_cascades_and_unresolved_zero_gain_gap(self):
        report = comparison.compare(self.library, self.captures)
        self.assertFalse(report["passed"])
        self.assertEqual(len(report["reports"]), 8)
        self.assertEqual(sum(len(r["checks"]) for r in report["reports"]), 230)
        self.assertEqual(sum(not c["passed"] for r in report["reports"] for c in r["checks"]), 22)
        for r in report["reports"]:
            self.assertEqual(r["passed"], r["configuration"]["second_gain_db"] == 10)
            self.assertTrue(
                all(c["passed"] for c in r["checks"] if c.get("measurement") == "RFPwrIn")
            )

    def test_narrow_channel_requires_bandwidth_normalization(self):
        narrow = next(c for c in self.captures if c["channel_bandwidth_hz"] == 1)
        self.assertTrue(comparison.compare(self.library, [narrow])["passed"])
        narrow["channel_bandwidth_hz"] = 1e6
        with self.assertRaisesRegex(ValueError, "ChanBW"):
            comparison.compare(self.library, [narrow])

    def test_topology_parameters_and_freshness_rejected(self):
        for mutation in (
            "topology",
            "gain",
            "clock",
            "order",
            "secondary",
            "stale-main",
            "stale-path",
            "warnings",
        ):
            c = copy.deepcopy(self.captures[0])
            if mutation == "topology":
                comparison.node(c, "Design3/PartList/RFAmp2")["netlist"] = "wrong"
            elif mutation == "gain":
                comparison.node(c, "Design3/PartList/RFAmp2/ParamSet/G")["data"] = 10
            elif mutation == "clock":
                comparison.node(c, "Design3/PartList/Source/ParamSet/RefClk")["data"] = "locked"
            elif mutation == "order":
                c["maximum_order"] = 2
            elif mutation == "secondary":
                c["secondary_spectrum"] = not c["secondary_spectrum"]
            elif mutation.startswith("stale"):
                suffix = (
                    "System3_Design3_Data"
                    if mutation == "stale-main"
                    else "System3_Data_Folder/System3_Design3_Data_Path1"
                )
                comparison.node(c, suffix)["timestamp"] = "0"
            else:
                c["manager_errors"] = "(WARNING) Maximum Order is 2 which has been increased to 3."
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                comparison.compare(self.library, [c])

    def test_missing_products_phase_and_shapes_detected(self):
        c = next(c for c in self.captures if c["two_tone"] and c["source_power_dbm"] == -30)
        original = self.library.coherent_amplifier

        def incomplete(*args, **kwargs):
            result = original(*args, **kwargs)
            return (
                result._replace(terms=result.terms[:-1])
                if kwargs.get("propagate_distortion")
                else result
            )

        with (
            patch.object(self.library, "coherent_amplifier", side_effect=incomplete),
            self.assertRaises(ValueError),
        ):
            comparison.compare(self.library, [c])
        voltages = comparison.node(c, comparison.SPECTRUM + "V2")
        voltages["data"] = [-v for v in voltages["data"]]
        self.assertFalse(comparison.compare(self.library, [c])["passed"])
        voltages["data"].pop()
        with self.assertRaises(ValueError):
            comparison.compare(self.library, [c])

    def test_duplicate_and_empty_not_accepted(self):
        for cases in ([], self.captures[:1] * 2):
            with self.assertRaises(ValueError):
                comparison.compare(self.library, cases)

    def test_archive_command_state_and_readback_are_consistent(self):
        c = self.captures[0]
        command = [
            "powershell.exe",
            "-WorkspacePath",
            r"D:\project\RFModel\build-reference\RFModel_CascadeIntermods.wsv",
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
            command.extend([flag, str(c[key])])
        status = dict(
            case="cascade",
            state="captured",
            collector_exit_code=0,
            eligible_for_compatibility=True,
            command=command,
        )
        self.assertEqual(
            archive.extract(c, status, c["raw_capture_sha256"])["source_power_dbm"],
            c["source_power_dbm"],
        )
        truncated = copy.deepcopy(c)
        comparison.node(truncated, comparison.SPECTRUM + "V3")["data"] = None
        with self.assertRaisesRegex(ValueError, "Malformed spectrum array"):
            archive.extract(truncated, status, truncated["raw_capture_sha256"])
        for changed in (
            dict(status, state="failed"),
            dict(status, eligible_for_compatibility=False),
            dict(status, command=command + ["-TwoTone"]),
        ):
            with self.assertRaises(ValueError):
                archive.extract(c, changed, c["raw_capture_sha256"])


if __name__ == "__main__":
    unittest.main()

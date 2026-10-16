"""Offline-only tests for the frozen RES input generator; no vendor dependency."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
GENERATOR = Path(__file__).with_name("prepare-systemvue-res-input.py")
PLAN = ROOT / "validation/systemvue-linear-rfsystem-minimal-plan-20261016.json"
PLAN_HASH = "4000c4549f5ed0a129f1f5ccf1069d4ef4c36be6da9c5de04d86b441f9d4a30e"


class RESInputTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="rfmodel-res-input-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def generate(self, output, plan=PLAN):
        return subprocess.run([sys.executable, "-I", "-S", str(GENERATOR),
                               "--plan", str(plan), "--output-dir", str(output)],
                              capture_output=True, text=True, check=False)

    def test_frozen_plan_generates_deterministic_disabled_input(self):
        self.assertEqual(hashlib.sha256(PLAN.read_bytes()).hexdigest(), PLAN_HASH)
        a, b = self.root / "first", self.root / "second"
        for directory in (a, b):
            result = self.generate(directory)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sorted(p.name for p in a.iterdir()),
                         ["input-generation.json", "res-input-uninitialized.wsv"])
        for path in a.iterdir():
            self.assertEqual(path.read_bytes(), (b / path.name).read_bytes())
        xml = (a / "res-input-uninitialized.wsv").read_bytes()
        receipt = json.loads((a / "input-generation.json").read_text(encoding="utf-8"))
        self.assertEqual(receipt["status"], "uninitialized_input_not_simulated")
        self.assertEqual(receipt["plan_sha256"], PLAN_HASH)
        self.assertEqual(receipt["input_sha256"], hashlib.sha256(xml).hexdigest())
        self.assertEqual(receipt["analysis_requests"], 0)
        tree = ET.fromstring(xml)
        designs = tree.find("Item[@XName='Designs']")
        parts = designs.find("Item[@XName='RES_Shared']/Item[@XName='PartList']")
        self.assertEqual([(p.attrib["XName"], p.findtext("Model"), p.findtext("Netlist"))
                          for p in parts],
                         [("Source", "MultiSource", "1,1"),
                          ("R1", "RES", "Port_1,1|Port_2,2"),
                          ("R2", "RES", "Port_1,2|Port_2,3"),
                          ("Load", "*OUT", "Port_1,3")])
        for name in ("RES_Linear", "RES_RFSystem"):
            analysis = designs.find("Item[@XName='%s']" % name)
            for field in ("AutoRecalc", "NeedRun", "Selected"):
                self.assertEqual(analysis.findtext(field + "/D"), "0")

    def test_changed_plan_rejected_before_creating_any_output(self):
        changed = json.loads(PLAN.read_bytes())
        changed["plan_id"] += "-different"
        altered_value = json.loads(PLAN.read_bytes())
        altered_value["schema_version"] = 999
        cases = {
            "plan_id": json.dumps(changed).encode(),
            "same_id_changed_field": json.dumps(altered_value).encode(),
            "same_json_changed_bytes": PLAN.read_bytes() + b"\n",
            "malformed": b"{",
            "missing_key": b"{}",
        }
        for name, data in cases.items():
            with self.subTest(name=name):
                plan = self.root / (name + ".json")
                plan.write_bytes(data)
                output = self.root / (name + "-parent") / "output"
                result = self.generate(output, plan)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(output.parent.exists())

    def test_missing_plan_rejected_without_output(self):
        output = self.root / "output"
        result = self.generate(output, self.root / "missing.json")
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())

    def test_existing_directory_or_file_is_never_overwritten(self):
        existing = self.root / "existing"
        existing.mkdir()
        sentinel = existing / "res-input-uninitialized.wsv"
        sentinel.write_bytes(b"existing evidence\x00")
        result = self.generate(existing)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(list(existing.iterdir()), [sentinel])
        self.assertEqual(sentinel.read_bytes(), b"existing evidence\x00")
        empty = self.root / "empty"
        empty.mkdir()
        self.assertNotEqual(self.generate(empty).returncode, 0)
        self.assertEqual(list(empty.iterdir()), [])
        file_path = self.root / "file"
        file_path.write_bytes(b"owned existing file")
        self.assertNotEqual(self.generate(file_path).returncode, 0)
        self.assertEqual(file_path.read_bytes(), b"owned existing file")


if __name__ == "__main__":
    unittest.main()

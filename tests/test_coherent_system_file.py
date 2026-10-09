"""Exercise complete coherent RF graphs, shared origins, and CLI input protection."""
import cmath
import copy
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

LIBRARY_PATH = Path(sys.argv.pop(1)).resolve()
ROOT = Path(__file__).resolve().parents[1]
INSTALLED = "--installed" in sys.argv
if INSTALLED:
    sys.argv.remove("--installed")
else:
    sys.path.insert(0, str(ROOT / "python"))
from rfmodel import Library, RFModelError
from rfmodel.coherent_system_file import analyze_coherent_system
from rfmodel.model_file import analyze, load, referenced_touchstone_paths


def fixture():
    return load(ROOT / "examples" / "coherent-image-rejection.json")


def stream(result, name="result"):
    return next(value for value in result["streams"] if value["id"] == name)


def powers(result, name="result"):
    return {value["bin"]: value["power_w"] for value in stream(result, name)["power_by_bin"]}


def post_network(model, device):
    model["stages"].append({
        "id": "post", "type": "linear_network",
        "network": {"devices": [dict(device, id="post")],
                    "external_ports": [["post", 0], ["post", 1]]},
        "inputs": [{"stream": "result", "port": ["post", 0]}],
        "outputs": [{"id": "filtered", "port": ["post", 1]}]})
    model["outputs"] = ["filtered"]


class CoherentSystemFileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def evaluate(self, model, **kwargs):
        return analyze_coherent_system(self.library, model, **kwargs)

    def test_split_mix_combine_and_one_extraction_per_network(self):
        model = fixture()
        original = copy.deepcopy(model)
        with patch("rfmodel.coherent_network_file.analyze", wraps=analyze) as linear:
            result = self.evaluate(model)
        self.assertEqual(linear.call_count, 2)
        self.assertEqual(model, original)
        self.assertEqual(result["outputs"], ["result"])
        self.assertEqual(len(result["streams"]), 6)
        for name, expected in (("rf-i", 1 / math.sqrt(2)), ("rf-q", 1j / math.sqrt(2))):
            value = stream(result, name)["components"][0]
            self.assertAlmostEqual(complex(*value["amplitude"]), expected)
            self.assertAlmostEqual(stream(result, name)["total_power_w"], .5)
        self.assertAlmostEqual(powers(result)[2], 1.)
        self.assertLess(powers(result)[18], 1e-28)
        self.assertEqual(stream(result, "mixed-i")["components"][0]["coherence_group"],
                         stream(result, "mixed-q")["components"][0]["coherence_group"])

    def test_independent_lo_and_independent_rf_prevent_cancellation(self):
        model = fixture()
        model["sources"][2].pop("reference_clock")
        result = self.evaluate(model)
        self.assertAlmostEqual(powers(result)[2], .5)
        self.assertAlmostEqual(powers(result)[18], .5)
        model = fixture()
        model["sources"].append({"id": "rf-other"})
        model["inputs"] = [
            {"id": "rf-i", "components": [{"source": "rf", "bin": 10,
                                          "bandwidth_hz": 1., "amplitude": 1 / math.sqrt(2)}]},
            {"id": "rf-q", "components": [{"source": "rf-other", "bin": 10,
                                          "bandwidth_hz": 1., "amplitude": [0., 1 / math.sqrt(2)]}]}]
        model["stages"].pop(0)
        result = self.evaluate(model)
        self.assertAlmostEqual(powers(result)[18], .5)

    def test_separate_banks_reuse_origins_and_independent_stage_order(self):
        model = fixture()
        branches = model["stages"][1]["branches"]
        model["stages"][1:2] = [
            {"id": "mix-i", "type": "ideal_mixer_bank", "branches": branches[:1]},
            {"id": "mix-q", "type": "ideal_mixer_bank", "branches": branches[1:]}]
        for reverse in (False, True):
            if reverse:
                model["stages"][1:3] = model["stages"][1:3][::-1]
            result = self.evaluate(model)
            self.assertAlmostEqual(powers(result)[2], 1.)
            self.assertLess(powers(result)[18], 1e-28)
            self.assertEqual(stream(result, "mixed-i")["components"][0]["coherence_group"],
                             stream(result, "mixed-q")["components"][0]["coherence_group"])

    def test_two_conversion_levels_preserve_new_origins(self):
        model = fixture()
        model["stages"].append({"id": "second-conversion", "type": "ideal_mixer_bank",
                                "branches": [{"id": "twice", "input": "result",
                                               "lo_source": "lo-a", "lo_bin": 1,
                                               "conversion_gain_db": 0}]})
        model["outputs"] = ["twice"]
        result = self.evaluate(model)
        self.assertAlmostEqual(powers(result, "twice")[1], 1.)
        self.assertAlmostEqual(powers(result, "twice")[3], 1.)
        original_group = stream(result)["components"][0]["coherence_group"]
        self.assertTrue(all(c["coherence_group"] != original_group
                            for c in stream(result, "twice")["components"]))

    def test_bypass_groups_do_not_collide_with_conversion_groups(self):
        model = fixture()
        model["inputs"].append({"id": "bypass", "components": [
            {"source": "rf", "bin": 2, "bandwidth_hz": 1., "amplitude": -1.}]})
        combiner = copy.deepcopy(model["stages"][-1])
        combiner["id"] = "bypass-combine"
        combiner["inputs"][0]["stream"] = "result"
        combiner["inputs"][1]["stream"] = "bypass"
        combiner["outputs"][0]["id"] = "with-bypass"
        model["stages"].append(combiner)
        model["outputs"] = ["with-bypass"]
        result = self.evaluate(model)
        self.assertAlmostEqual(powers(result, "with-bypass")[2], 1.)

    def test_frequency_dependent_post_network_on_generated_bins(self):
        model = fixture()
        model["inputs"][0]["components"].append(
            dict(model["inputs"][0]["components"][0], bin=14))
        post_network(model, {"model": {"type": "transmission_line",
                                       "characteristic_ohms": 50, "delay_s": 1e-9}})
        result = self.evaluate(model)
        values = {c["bin"]: complex(*c["amplitude"])
                  for c in stream(result, "filtered")["components"]}
        self.assertEqual(set(values), {2, 6, 18, 22})
        for index in (2, 6):
            self.assertAlmostEqual(values[index], cmath.exp(-2j * math.pi * index * 1e8 * 1e-9))
        for index in (18, 22):
            self.assertLess(abs(values[index]), 1e-14)

    def test_empty_streams_validate_topology_and_lo(self):
        model = fixture()
        model["inputs"][0]["components"] = []
        self.assertEqual(stream(self.evaluate(model))["total_power_w"], 0)
        for kind in ("input_port", "lo", "output_port"):
            bad = copy.deepcopy(model)
            if kind == "input_port":
                bad["stages"][0]["inputs"][0]["port"] = ["s", 999]
            elif kind == "lo":
                bad["stages"][1]["branches"][0]["lo_bin"] = 0
            else:
                bad["stages"][0]["outputs"][0]["port"] = ["missing", 1]
            with self.subTest(kind=kind), self.assertRaises((ValueError, RFModelError)):
                self.evaluate(bad)

    def test_invalid_graphs_fail_without_mutating_model(self):
        changes = [
            lambda m: m.update(version=True),
            lambda m: m.update(spacing_hz=float("nan")),
            lambda m: m.update(reference_ohms=0),
            lambda m: m.update(outputs=["missing"]),
            lambda m: m.update(outputs=["result", "result"]),
            lambda m: m["inputs"].append(copy.deepcopy(m["inputs"][0])),
            lambda m: m["stages"][0].update(id=m["stages"][1]["id"]),
            lambda m: m["stages"][0]["inputs"][0].update(stream="result"),
            lambda m: m["stages"][0]["outputs"][0].update(id="incident"),
            lambda m: m["stages"][1]["branches"][0].update(lo_source="missing"),
            lambda m: m["stages"][1]["branches"][0].update(lo_bin=10),
            lambda m: m["stages"][1]["branches"][0].update(lo_bin=True),
            lambda m: m["stages"][0]["network"]["devices"][0].update(noise={}),
            lambda m: m["stages"][0].update(type="nonlinear-feedback"),
            lambda m: m["inputs"][0]["components"][0].update(coherence_group=1),
            lambda m: m["stages"][0]["outputs"][1].update(port=["s", 1]),
            lambda m: m["stages"][0].update(id="bad" + chr(0)),
        ]
        for index, change in enumerate(changes):
            model = fixture()
            change(model)
            before = copy.deepcopy(model)
            with self.subTest(index=index), self.assertRaises((ValueError, TypeError, RFModelError)):
                self.evaluate(model)
            self.assertEqual(repr(model), repr(before))

    def test_work_limits(self):
        model = fixture()
        model["stages"] = model["stages"] * 171
        with self.assertRaises(ValueError):
            self.evaluate(model)
        model = fixture()
        model["stages"][1]["branches"] *= 1025
        with self.assertRaises(ValueError):
            self.evaluate(model)

    def test_touchstone_cli_and_failure_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            data, model_file, output = folder / "sample.s2p", folder / "graph.json", folder / "out.json"
            raw = b"# GHz S RI R 50\n0.2 0 0 .5 0 .5 0 0 0\n2.2 0 0 .25 0 .25 0 0 0\n"
            data.write_bytes(raw)
            model = fixture()
            model["inputs"][0]["components"].append(dict(model["inputs"][0]["components"][0], bin=14))
            post_network(model, {"model": {"type": "touchstone", "path": "sample.s2p"}})
            result = self.evaluate(model, base_directory=folder)
            self.assertAlmostEqual(powers(result, "filtered")[2], .25)
            self.assertAlmostEqual(powers(result, "filtered")[6], .2025)
            self.assertEqual(referenced_touchstone_paths(model, folder), {data.resolve()})
            model_file.write_text(json.dumps(model), encoding="utf-8")
            env = dict(os.environ) if INSTALLED else dict(os.environ, PYTHONPATH=str(ROOT / "python"))
            command = [sys.executable, "-m", "rfmodel", str(model_file), "--library", str(LIBRARY_PATH),
                       "--output"]
            run = subprocess.run(command + [str(output)], env=env, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(json.loads(output.read_text()), result)
            saved = output.read_bytes()
            run = subprocess.run(command + [str(data)], env=env, capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn("must not overwrite Touchstone", run.stderr)
            self.assertEqual(data.read_bytes(), raw)
            model["stages"][1]["branches"][0]["lo_bin"] = 0
            model_file.write_text(json.dumps(model), encoding="utf-8")
            run = subprocess.run(command + [str(output)], env=env, capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()

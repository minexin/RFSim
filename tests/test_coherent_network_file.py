"""Exercise coherent-network JSON and CLI against the native library."""
import copy
import json
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
from rfmodel.coherent_network_file import analyze_coherent_network
from rfmodel.model_file import load, referenced_touchstone_paths


def component(bin=10, amplitude=1., group=7):
    return {"bin": bin, "kind": "source", "bandwidth_hz": 1.,
            "coherence_group": group, "amplitude": amplitude}


def combiner():
    return {"format": "rfmodel.coherent-network", "version": 1, "spacing_hz": 1e8,
            "network": {"devices": [{"id": "c", "s": [[0, 0, .5], [0, 0, -.5], [.5, -.5, 0]]}],
                        "external_ports": [["c", 2], ["c", 1], ["c", 0]]},
            "inputs": [{"port": ["c", 0], "component": component()},
                       {"port": ["c", 1], "component": component(amplitude=-1.)}],
            "output_port": ["c", 2]}


class CoherentNetworkFileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def evaluate(self, model, **kwargs):
        return analyze_coherent_network(self.library, model, **kwargs)

    def test_source_definitions_resolve_clock_relationships(self):
        model = combiner()
        model["sources"] = [{"id": "a", "reference_clock": "clock"},
                            {"id": "b", "reference_clock": "clock"}]
        for value, source in zip(model["inputs"], ("a", "b")):
            value["component"].pop("kind")
            value["component"].pop("coherence_group")
            value["component"]["source"] = source
        result = self.evaluate(model)
        self.assertEqual(result["total_power_w"], 1.)
        self.assertEqual(result["sources"][0]["coherence_group"], result["sources"][1]["coherence_group"])
        model["sources"][1].pop("reference_clock")
        result = self.evaluate(model)
        self.assertEqual(result["total_power_w"], .5)
        self.assertNotEqual(result["sources"][0]["coherence_group"], result["sources"][1]["coherence_group"])
        model["inputs"][1]["component"]["source"] = "a"
        self.assertEqual(self.evaluate(model)["total_power_w"], 1.)
        for mutate in (
                lambda m: m["inputs"][0]["component"].update(coherence_group=1),
                lambda m: m["inputs"][0]["component"].update(kind="harmonic"),
                lambda m: m["inputs"][0]["component"].update(source="missing"),
                lambda m: m["sources"].append({"id": "a"}),
                lambda m: m["sources"][0].update(reference_clock=True)):
            bad = copy.deepcopy(model)
            mutate(bad)
            with self.assertRaises((ValueError, TypeError, RFModelError)):
                self.evaluate(bad)

    def test_split_delay_recombine_at_actual_frequencies(self):
        model = load(ROOT / "examples/coherent-split-delay.json")
        original = copy.deepcopy(model)
        result = self.evaluate(model)
        self.assertEqual(model, original)
        self.assertEqual([p["bin"] for p in result["power_by_bin"]], [10, 20])
        self.assertLess(result["power_by_bin"][0]["power_w"], 1e-28)
        self.assertAlmostEqual(result["power_by_bin"][1]["power_w"], 1.)
        self.assertEqual(len(result["components"]), 2)
        self.assertEqual(result["output_port"], ["combine", 0])

    def test_ports_are_not_merged_before_different_transfer_paths(self):
        model = combiner()
        result = self.evaluate(model)
        self.assertEqual(result["total_power_w"], 1.)
        self.assertEqual(result["components"][0]["amplitude"], [1., 0.])
        model["network"]["external_ports"].reverse()
        self.assertEqual(self.evaluate(model)["components"], result["components"])
        model["inputs"][1]["component"]["coherence_group"] = 8
        result = self.evaluate(model)
        self.assertEqual(result["total_power_w"], .5)
        self.assertEqual(len(result["components"]), 2)

    def test_duplicate_port_groups_and_same_port_observation(self):
        model = combiner()
        model["inputs"].append({"port": ["c", 0], "component": component(amplitude=-1.)})
        self.assertEqual(self.evaluate(model)["total_power_w"], .25)
        model["inputs"] = [{"port": ["c", 2], "component": component()}]
        model["network"]["devices"][0]["s"][2][2] = -.25
        self.assertEqual(self.evaluate(model)["components"][0]["amplitude"], [-.25, 0.])

    def test_internal_reflection_and_termination(self):
        model = combiner()
        model["network"] = {
            "devices": [{"id": "a", "s": [[0, .5], [.5, .2]]},
                        {"id": "b", "s": [[.3, .4], [.4, 0]]},
                        {"id": "unused", "s": [[.1]]}],
            "connections": [[["a", 1], ["b", 0]]],
            "terminations": [{"port": ["unused", 0], "reflection": .5}],
            "external_ports": [["b", 1], ["a", 0]]}
        model["inputs"] = [{"port": ["a", 0], "component": component(amplitude=[0, 1])}]
        model["output_port"] = ["b", 1]
        result = self.evaluate(model)
        self.assertAlmostEqual(result["components"][0]["amplitude"][1], .2/.94)
        self.assertAlmostEqual(result["total_power_w"], (.2/.94)**2)
        model["network"]["terminations"] = []
        with self.assertRaises(RFModelError):
            self.evaluate(model)

    def test_strict_schema_ports_metadata_and_resource_limits(self):
        mutations = [
            lambda m: m.update(version=True),
            lambda m: m.update(spacing_hz=0),
            lambda m: m.update(reference_ohms=-1),
            lambda m: m.update(noise=[]),
            lambda m: m.update(output_port=["c", True]),
            lambda m: m.update(output_port=["c", 99]),
            lambda m: m["network"].update(temperature_k=290),
            lambda m: m["network"].update(signal_boundaries=[]),
            lambda m: m["network"]["devices"][0].update(noise={"noiseless": True}),
            lambda m: m["network"]["devices"][0].update(s_samples=[]),
            lambda m: m["network"].update(external_ports=[["c", 2], ["c", 2]]),
            lambda m: m["inputs"][0].update(port=["unknown", 0]),
            lambda m: m["inputs"][0]["component"].update(coherence_group=True),
            lambda m: m["inputs"][0]["component"].update(bin=0),
            lambda m: m["inputs"][0]["component"].update(bandwidth_hz=0),
            lambda m: m["inputs"][0]["component"].update(amplitude=float("nan")),
            lambda m: m.update(inputs=m["inputs"] * 2049),
            lambda m: m.update(inputs=[
                {"port": ["c", 0], "component": component(bin=i)} for i in range(1, 2050)]),
        ]
        for index, mutate in enumerate(mutations):
            model = combiner()
            mutate(model)
            with self.subTest(index=index):
                with self.assertRaises((ValueError, TypeError, RFModelError)):
                    self.evaluate(model)
        model = combiner()
        mutations[-1](model)
        with patch("rfmodel.coherent_network_file.analyze") as linear:
            with self.assertRaises(ValueError):
                self.evaluate(model)
            linear.assert_not_called()

    def test_empty_input_still_validates_topology(self):
        model = combiner()
        model["inputs"] = []
        result = self.evaluate(model)
        self.assertEqual(result["components"], [])
        self.assertEqual(result["total_power_w"], 0)
        model["network"]["external_ports"].append(["unknown", 0])
        with self.assertRaises(ValueError):
            self.evaluate(model)

    def test_touchstone_interpolation_relative_paths_and_empty_dc_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory) / "sample.s2p"
            data.write_text("# GHz S RI R 50\n1 0 0 0 .5 0 .5 0 0\n2 0 0 -.25 0 -.25 0 0 0\n",
                            encoding="utf-8")
            model = combiner()
            model["network"] = {
                "devices": [{"id": "t", "model": {"type": "touchstone", "path": "sample.s2p"}}],
                "external_ports": [["t", 1], ["t", 0]]}
            model["inputs"] = [{"port": ["t", 0], "component": component(bin=i)} for i in (10, 15, 20)]
            model["output_port"] = ["t", 1]
            result = self.evaluate(model, base_directory=directory)
            for value, expected in zip(result["components"], (complex(0, .5), complex(-.125, .25), -.25)):
                self.assertAlmostEqual(complex(*value["amplitude"]), expected)
            self.assertEqual(referenced_touchstone_paths(model, directory), {data.resolve()})
            model["inputs"] = []
            with self.assertRaises(RFModelError):
                self.evaluate(model, base_directory=directory)
            model["network"]["devices"][0]["model"]["out_of_band"] = "clamp"
            self.assertEqual(self.evaluate(model, base_directory=directory)["total_power_w"], 0)

    def test_cli_runs_and_does_not_overwrite_touchstone_or_result_on_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            data, source, output = directory / "data.s1p", directory / "model.json", directory / "out.json"
            original = b"# GHz S RI R 50\n1 .5 0\n"
            data.write_bytes(original)
            model = combiner()
            model["network"] = {"devices": [{"id": "r", "model": {
                "type": "touchstone", "path": "data.s1p"}}], "external_ports": [["r", 0]]}
            model["output_port"] = ["r", 0]
            model["inputs"] = [{"port": ["r", 0], "component": component()}]
            source.write_text(json.dumps(model), encoding="utf-8")
            command = [sys.executable, "-m", "rfmodel", str(source), "--library", str(LIBRARY_PATH), "--output"]
            env = dict(os.environ) if INSTALLED else dict(os.environ, PYTHONPATH=str(ROOT / "python"))
            run = subprocess.run(command + [str(output)], env=env, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(json.loads(output.read_text())["total_power_w"], .25)
            run = subprocess.run(command + [str(data)], env=env, capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn("must not overwrite Touchstone", run.stderr)
            self.assertEqual(data.read_bytes(), original)
            result_bytes = output.read_bytes()
            model["inputs"][0]["component"]["coherence_group"] = 0
            source.write_text(json.dumps(model), encoding="utf-8")
            run = subprocess.run(command + [str(output)], env=env, capture_output=True, text=True)
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(output.read_bytes(), result_bytes)


if __name__ == "__main__":
    unittest.main()

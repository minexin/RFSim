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

    def test_cascade_combines_conducted_and_generated_origins(self):
        model = load(ROOT / "examples/coherent-amplifier-cascade.json")
        result = self.evaluate(model)
        first, second = result["stages"]
        self.assertEqual(len(first["origins"]), 16)
        self.assertEqual(len(second["origins"]), 30)
        self.assertEqual(len(stream(result, "second")["components"]), 16)
        first_groups = {o["coherence_group"] for o in first["origins"]}
        self.assertEqual({o["coherence_group"] for o in second["origins"]}, first_groups)
        self.assertAlmostEqual(second["input_power_w"], stream(result, "first")["total_power_w"])
        # Compare each merged product with its two independently calculated paths.
        from rfmodel import CoherentComponent, SpectrumKind
        inputs = [CoherentComponent(c["bin"], SpectrumKind[c["kind"].upper()],
                                    c["bandwidth_hz"], c["coherence_group"], complex(*c["amplitude"]))
                  for c in stream(result, "first")["components"]]
        native = self.library.coherent_amplifier(
            1e8, inputs, propagate_distortion=True, power_gain_db=10.,
            output_p1db_dbm=20., output_saturation_dbm=23., input_ip2_dbm=30., input_ip3_dbm=20.)
        by_group = {}
        for t, origin in zip(native.terms, second["origins"]):
            group = origin["coherence_group"]
            by_group[group] = by_group.get(group, 0j) + t.component.amplitude
        for c in stream(result, "second")["components"]:
            self.assertAlmostEqual(complex(*c["amplitude"]), by_group[c["coherence_group"]])
        # A third primary-generation stage must reuse the same root identities.
        model["stages"].append(dict(model["stages"][1], id="third", input="second", output="third"))
        model["outputs"] = ["third"]
        third = self.evaluate(model)
        self.assertEqual(len(stream(third, "third")["components"]), 16)
        self.assertEqual({c["coherence_group"] for c in stream(third, "third")["components"]},
                         first_groups)

    def test_parallel_amplifiers_share_origins_and_preserve_parity(self):
        model = load(ROOT / "examples/coherent-harmonic-combiner.json")
        original = copy.deepcopy(model)
        for reverse in (False, True):
            if reverse:
                model["stages"][:2] = model["stages"][1::-1]
            result = self.evaluate(model)
            self.assertEqual(len(stream(result)["components"]), 4)
            output = powers(result)
            self.assertLess(output[10], 1e-30)
            self.assertLess(output[30], 1e-30)
            self.assertAlmostEqual(output[20], powers(result, "out-positive")[20])
            first, second = result["stages"][:2]
            self.assertEqual(first["origins"], second["origins"])
            self.assertEqual(first["reduced_inputs"][0]["bin"], 10)
        self.assertEqual(original["inputs"], model["inputs"])
        model["sources"].append({"id": "other"})
        model["inputs"][1]["components"][0]["source"] = "other"
        independent = self.evaluate(model)
        self.assertEqual(len(stream(independent)["components"]), 8)
        for bin_index in (10, 20, 30):
            self.assertAlmostEqual(powers(independent)[bin_index],
                                   powers(independent, "out-positive")[bin_index] / 2)
        model["sources"][0]["reference_clock"] = "same"
        model["sources"][1]["reference_clock"] = "same"
        self.assertLess(powers(self.evaluate(model))[10], 1e-30)

    def test_amplifier_origin_registry_uses_parent_keys_not_local_indices(self):
        model = load(ROOT / "examples/coherent-harmonic-combiner.json")
        model["inputs"][0]["components"].append(
            {"source": "rf", "bin": 11, "bandwidth_hz": 1., "amplitude": .01})
        model["inputs"][1]["components"][0].update(bin=11, amplitude=.01)
        result = self.evaluate(model)
        left = stream(result, "out-positive")["components"]
        right = stream(result, "out-negative")["components"]

        def harmonic(values, bin_index):
            return next(c for c in values if c["bin"] == bin_index and c["kind"] == "harmonic")

        self.assertEqual(harmonic(left, 22)["coherence_group"],
                         harmonic(right, 22)["coherence_group"])
        self.assertNotEqual(harmonic(left, 20)["coherence_group"],
                            harmonic(right, 22)["coherence_group"])
        # Different bandwidth is also a different generating identity.
        model["inputs"][1]["components"][0]["bandwidth_hz"] = 2.
        changed = self.evaluate(model)
        self.assertNotEqual(harmonic(stream(changed, "out-positive")["components"], 22)["coherence_group"],
                            harmonic(stream(changed, "out-negative")["components"], 22)["coherence_group"])

    def test_generated_harmonics_and_intermods_continue_through_mixer(self):
        model = load(ROOT / "examples/coherent-harmonic-combiner.json")
        model["sources"].append({"id": "lo"})
        model["stages"] = model["stages"][:1] + [{
            "id": "downconvert", "type": "ideal_mixer_bank",
            "branches": [{"id": "if", "input": "out-positive", "lo_source": "lo",
                          "lo_bin": 8, "conversion_gain_db": 0.}]}]
        model["outputs"] = ["if"]
        result = self.evaluate(model)
        before = stream(result, "out-positive")
        after = stream(result, "if")
        self.assertEqual(len(after["components"]), 8)
        self.assertEqual(set(powers(result, "if")), {2, 12, 18, 22, 28, 38})
        self.assertAlmostEqual(after["total_power_w"], 2*before["total_power_w"])
        for c in before["components"]:
            matches = [v for v in after["components"]
                       if v["kind"] == c["kind"] and v["bandwidth_hz"] == c["bandwidth_hz"]]
            self.assertEqual(len(matches), 2)
            self.assertEqual(matches[0]["coherence_group"], matches[1]["coherence_group"])

    def test_amplifier_rejects_recursive_distortion_and_unknown_fields(self):
        model = load(ROOT / "examples/coherent-harmonic-combiner.json")
        recursive = dict(model["stages"][0], id="recursive", input="result", output="cascade")
        model["stages"].append(recursive)
        model["outputs"] = ["cascade"]
        with self.assertRaisesRegex(RFModelError, "source-kind"):
            self.evaluate(model)
        model["stages"].pop()
        model["outputs"] = ["result"]
        model["stages"][0]["undocumented"] = True
        with self.assertRaises(ValueError):
            self.evaluate(model)

    def test_compression_before_split_mix_combine_preserves_cancellation(self):
        model = load(ROOT / "examples/coherent-compressed-receiver.json")
        original = copy.deepcopy(model)
        result = self.evaluate(model)
        self.assertEqual(model, original)
        expected = 10**((23 - 30) / 10)
        self.assertAlmostEqual(result["stages"][0]["input_power_w"], 1.)
        self.assertAlmostEqual(stream(result, "compressed-rf")["total_power_w"], expected)
        self.assertAlmostEqual(powers(result)[2], expected)
        self.assertLess(powers(result)[18], 1e-28)
        group = stream(result, "compressed-rf")["components"][0]["coherence_group"]
        self.assertEqual(group, stream(result, "rf-i")["components"][0]["coherence_group"])
        model["sources"][2].pop("reference_clock")
        independent = self.evaluate(model)
        self.assertAlmostEqual(powers(independent)[2], expected / 2)
        self.assertAlmostEqual(powers(independent)[18], expected / 2)

    def test_shared_compression_blocker_and_coherent_cancellation(self):
        anchor = 10**(-2.9)
        wave = math.sqrt(anchor / 2)
        model = {"format": "rfmodel.coherent-system", "version": 1, "spacing_hz": 1e8,
                 "sources": [{"id": "a"}, {"id": "b"}],
                 "inputs": [{"id": "rf", "components": [
                     {"source": "a", "bin": 10, "bandwidth_hz": 1, "amplitude": wave},
                     {"source": "b", "bin": 11, "bandwidth_hz": 1, "amplitude": [0, wave]}]}],
                 "stages": [{"id": "amp", "type": "fundamental_compression", "input": "rf",
                             "output": "out", "power_gain_db": 20., "output_p1db_dbm": 20.,
                             "output_saturation_dbm": 23.}], "outputs": ["out"]}
        result = self.evaluate(model)
        self.assertAlmostEqual(powers(result, "out")[10], .05)
        self.assertAlmostEqual(powers(result, "out")[11], .05)
        second = model["inputs"][0]["components"].pop()
        unblocked = self.evaluate(model)
        self.assertGreater(powers(unblocked, "out")[10], .05)
        second.update(source="a", bin=10, amplitude=-wave)
        model["inputs"][0]["components"].append(second)
        cancelled = self.evaluate(model)
        self.assertEqual(stream(cancelled, "out")["total_power_w"], 0.)
        self.assertEqual(cancelled["stages"][0]["input_power_w"], 0.)
        self.assertEqual(len(stream(cancelled, "out")["components"]), 1)

    def test_empty_compression_validates_parameters_and_graph_contract(self):
        model = load(ROOT / "examples/coherent-compressed-receiver.json")
        model["inputs"][0]["components"] = []
        self.assertEqual(stream(self.evaluate(model))["total_power_w"], 0.)
        for update in ({"output_saturation_dbm": 20}, {"power_gain_db": float("nan")},
                       {"unexpected": 1}, {"input": "future"}, {"output": model["inputs"][0]["id"]}):
            invalid = copy.deepcopy(model)
            invalid["stages"][0].update(update)
            with self.subTest(update=update), self.assertRaises((ValueError, RFModelError)):
                self.evaluate(invalid)

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

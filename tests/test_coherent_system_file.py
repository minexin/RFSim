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

    def mixed_model(self):
        return load(ROOT / "examples/coherent-mixed-polynomial.json")

    def assert_history(self, result):
        roots = {root["root_id"]: root for root in result["origin_roots"]}
        for value in result["streams"]:
            for component, origin in zip(value["components"], value["origins"]):
                self.assertEqual(component["coherence_group"], origin["coherence_group"])
                wave = sum(complex(*term["amplitude"]) for term in origin["terms"])
                self.assertAlmostEqual(wave, complex(*component["amplitude"]))
                for term in origin["terms"]:
                    factors = term["source_factors"]
                    self.assertEqual(term["source_order"], len(factors))
                    self.assertEqual(
                        sum(f["sign"] * roots[f["root_id"]]["bin"] for f in factors),
                        component["bin"],
                    )
                    self.assertAlmostEqual(
                        math.fsum(roots[f["root_id"]]["bandwidth_hz"] for f in factors),
                        component["bandwidth_hz"],
                    )
                    self.assertEqual(
                        term["rf_order"], sum(roots[f["root_id"]]["role"] == "rf" for f in factors)
                    )

    def test_mixed_polynomial_matches_independent_spectrum(self):
        model = self.mixed_model()
        original = copy.deepcopy(model)
        result = self.evaluate(model)
        self.assertEqual(model, original)
        expected_mixed = self.library.ideal_mixer(
            1e8,
            {8: 0.01 + 0.003j, 12: 0.02 - 0.002j},
            lo_bin=10,
            conversion_gain_db=0,
            lo_phase_radians=0.3,
        )
        expected = self.library.polynomial_amplifier(
            1e8, expected_mixed, voltage_coefficients=[0, 1, 0.5]
        )
        expected.pop(0, None)
        actual = {}
        for c in stream(result)["components"]:
            actual[c["bin"]] = actual.get(c["bin"], 0j) + complex(*c["amplitude"])
        self.assertEqual(set(actual), set(expected))
        for index in expected:
            self.assertLessEqual(
                abs(actual[index] - expected[index]), 1e-16 + 1e-11 * abs(expected[index])
            )
        folded = next(c for c in stream(result, "mixed")["components"] if c["bin"] == 2)
        folded_origin = next(o for o in stream(result, "mixed")["origins"] if o["bin"] == 2)
        self.assertEqual(len(folded_origin["terms"]), 2)
        self.assertAlmostEqual(complex(*folded["amplitude"]), expected_mixed[2])
        self.assertAlmostEqual(powers(result, "mixed")[2], abs(expected_mixed[2]) ** 2)
        harmonic = next(
            o for o in stream(result)["origins"] if o["bin"] == 4 and o["kind"] == "harmonic"
        )
        self.assertEqual(len(harmonic["terms"]), 3)
        self.assertEqual({t["source_order"] for t in harmonic["terms"]}, {4})
        self.assertEqual(result["source_order_policy"], "rf-and-lo-factors")
        self.assert_history(result)

    def test_mixed_cancellation_retains_nonlinear_cross_contributions(self):
        model = self.mixed_model()
        model["inputs"][0]["components"][0]["amplitude"] = 0.01
        model["inputs"][0]["components"][1]["amplitude"] = -0.01
        model["stages"][0]["branches"][0]["lo_phase_radians"] = 0
        model["stages"][1]["voltage_coefficients"] = [0, 0, 0.5]
        result = self.evaluate(model)
        self.assertEqual(powers(result, "mixed")[2], 0)
        harmonic = next(
            o for o in stream(result)["origins"] if o["bin"] == 4 and o["kind"] == "harmonic"
        )
        self.assertEqual(len(harmonic["terms"]), 3)
        self.assertAlmostEqual(sum(complex(*t["amplitude"]) for t in harmonic["terms"]), 0)
        self.assertTrue(all(abs(complex(*t["amplitude"])) > 0 for t in harmonic["terms"]))
        self.assert_history(result)

    def test_mixed_origins_are_local_to_streams_and_linear_paths(self):
        model = self.mixed_model()
        left, right = model["inputs"][0]["components"]
        model["inputs"] = [
            {"id": "left", "components": [left]},
            {"id": "right", "components": [right]},
        ]
        branch = model["stages"][0]["branches"][0]
        model["stages"][0]["branches"] = [
            dict(branch, id="a", input="left"),
            dict(branch, id="b", input="right"),
        ]
        model["stages"].insert(
            1,
            {
                "id": "combine",
                "type": "linear_network",
                "network": {
                    "devices": [{"id": "n", "s": [[0, 0, 0], [0, 0, 0], [0.5, [0, 0.5], 0]]}],
                    "external_ports": [["n", 0], ["n", 1], ["n", 2]],
                },
                "inputs": [{"stream": "a", "port": ["n", 0]}, {"stream": "b", "port": ["n", 1]}],
                "outputs": [{"id": "mixed", "port": ["n", 2]}],
            },
        )
        result = self.evaluate(model)
        a = next(o for o in stream(result, "a")["origins"] if o["bin"] == 2)
        b = next(o for o in stream(result, "b")["origins"] if o["bin"] == 2)
        self.assertEqual(a["coherence_group"], b["coherence_group"])
        self.assertNotEqual(a["terms"][0]["source_factors"], b["terms"][0]["source_factors"])
        self.assertEqual(len(a["terms"]), 1)
        self.assertEqual(len(b["terms"]), 1)
        expected = 0.5 * (0.01 - 0.003j) * cmath.exp(0.3j) + 0.5j * (0.02 - 0.002j) * cmath.exp(
            -0.3j
        )
        self.assertAlmostEqual(powers(result, "mixed")[2], abs(expected) ** 2)
        combined = next(o for o in stream(result, "mixed")["origins"] if o["bin"] == 2)
        self.assertEqual(len(combined["terms"]), 2)
        self.assert_history(result)

    def test_mixed_recursive_mixer_conjugates_entire_history(self):
        model = self.mixed_model()
        model["sources"].append({"id": "lo2"})
        model["stages"].append(
            {
                "id": "again",
                "type": "ideal_mixer_bank",
                "branches": [
                    {
                        "id": "again",
                        "input": "result",
                        "lo_source": "lo2",
                        "lo_bin": 101,
                        "conversion_gain_db": -2,
                        "lo_phase_radians": -0.4,
                    }
                ],
            }
        )
        model["outputs"] = ["again"]
        result = self.evaluate(model)
        physical = {}
        for component in stream(result)["components"]:
            physical[component["bin"]] = physical.get(component["bin"], 0j) + complex(
                *component["amplitude"]
            )
        expected = self.library.ideal_mixer(
            1e8, physical, lo_bin=101, conversion_gain_db=-2, lo_phase_radians=-0.4
        )
        actual = {}
        for component in stream(result, "again")["components"]:
            actual[component["bin"]] = actual.get(component["bin"], 0j) + complex(
                *component["amplitude"]
            )
        self.assertEqual(set(actual), set(expected))
        for index in expected:
            self.assertLessEqual(
                abs(actual[index] - expected[index]), 1e-16 + 1e-11 * abs(expected[index])
            )
        before = next(
            o for o in stream(result)["origins"] if o["bin"] == 4 and o["kind"] == "harmonic"
        )
        after = next(
            o
            for o in stream(result, "again")["origins"]
            if o["bin"] == 97 and o["kind"] == "harmonic"
        )
        for old in before["terms"]:
            negated = sorted((f["root_id"], -f["sign"]) for f in old["source_factors"])
            self.assertTrue(
                any(
                    sorted(
                        (f["root_id"], f["sign"])
                        for f in new["source_factors"]
                        if f["root_id"] <= 3
                    )
                    == negated
                    for new in after["terms"]
                )
            )
        self.assert_history(result)

    def test_mixed_source_order_counts_lo_and_filters_each_contribution(self):
        model = self.mixed_model()
        model["stages"][1]["max_source_order"] = 3
        result = self.evaluate(model)
        self.assertGreater(result["stages"][1]["discarded_term_count"], 0)
        self.assertEqual(result["stages"][1]["discarded_by_source_order"][0]["source_order"], 4)
        self.assertEqual(stream(result)["power_by_bin"], stream(result, "mixed")["power_by_bin"])
        model["stages"][1]["max_source_order"] = 1
        self.assertEqual(stream(self.evaluate(model))["components"], [])

    def test_polynomial_mixer_polynomial_matches_independent_chain(self):
        model = self.mixed_model()
        model["stages"].insert(
            0,
            {
                "id": "pre",
                "type": "polynomial_amplifier",
                "input": "input",
                "output": "pre",
                "voltage_coefficients": [0, 1, 0.1],
                "max_source_order": 2,
            },
        )
        model["stages"][1]["branches"][0]["input"] = "pre"
        model["stages"][1]["branches"][0]["lo_bin"] = 101
        model["stages"][2]["max_source_order"] = 6
        result = self.evaluate(model)
        expected = self.library.polynomial_amplifier(
            1e8, {8: 0.01 + 0.003j, 12: 0.02 - 0.002j}, voltage_coefficients=[0, 1, 0.1]
        )
        expected.pop(0, None)
        expected = self.library.ideal_mixer(
            1e8, expected, lo_bin=101, conversion_gain_db=0, lo_phase_radians=0.3
        )
        expected = self.library.polynomial_amplifier(
            1e8, expected, voltage_coefficients=[0, 1, 0.5]
        )
        expected.pop(0, None)
        actual = {}
        for c in stream(result)["components"]:
            actual[c["bin"]] = actual.get(c["bin"], 0j) + complex(*c["amplitude"])
        self.assertEqual(set(actual), set(expected))
        for index in expected:
            self.assertLessEqual(
                abs(actual[index] - expected[index]), 1e-16 + 1e-11 * abs(expected[index])
            )
        self.assert_history(result)

    def test_mixed_graph_retains_calibrated_compression_boundary(self):
        model = self.mixed_model()
        model["stages"].append({"id": "compression", "type": "fundamental_compression"})
        with self.assertRaisesRegex(ValueError, "calibrated amplifier"):
            self.evaluate(model)

    def test_mixed_graph_cli_and_failure_preserve_existing_result(self):
        model = self.mixed_model()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "mixed.json"
            output = Path(directory) / "result.json"
            source.write_text(json.dumps(model), encoding="utf-8")
            env = (
                dict(os.environ) if INSTALLED else dict(os.environ, PYTHONPATH=str(ROOT / "python"))
            )
            command = [
                sys.executable,
                "-m",
                "rfmodel",
                str(source),
                "--library",
                str(LIBRARY_PATH),
                "--output",
                str(output),
            ]
            completed = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(json.loads(output.read_text()), self.evaluate(model))
            saved = output.read_bytes()
            model["stages"][0]["branches"][0]["lo_bin"] = 8
            source.write_text(json.dumps(model), encoding="utf-8")
            completed = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertNotEqual(completed.returncode, 0)
            self.assertEqual(output.read_bytes(), saved)

    def test_mixed_polynomial_preserves_independent_coherence_classes(self):
        model = self.mixed_model()
        model["sources"].append({"id": "other"})
        model["inputs"][0]["components"][1]["source"] = "other"
        model["stages"][1]["voltage_coefficients"] = [0, 1]
        result = self.evaluate(model)
        folded = [c for c in stream(result)["components"] if c["bin"] == 2]
        self.assertEqual(len(folded), 2)
        self.assertNotEqual(folded[0]["coherence_group"], folded[1]["coherence_group"])
        self.assertAlmostEqual(powers(result)[2], abs(0.01 + 0.003j) ** 2 + abs(0.02 - 0.002j) ** 2)
        self.assert_history(result)

    def test_mixed_linear_polynomial_preserves_existing_image_rejection(self):
        model = fixture()
        original = self.evaluate(model)
        model["stages"].append(
            {
                "id": "poly",
                "type": "polynomial_amplifier",
                "input": "result",
                "output": "poly",
                "voltage_coefficients": [0, 1],
                "max_source_order": 4,
            }
        )
        model["outputs"] = ["poly"]
        result = self.evaluate(model)
        for index, power in powers(original).items():
            self.assertAlmostEqual(powers(result, "poly")[index], power, places=14)
        self.assert_history(result)

    def test_mixed_expanded_contribution_limit_and_silent_validation(self):
        model = self.mixed_model()
        model["inputs"][0]["components"] = [
            {"source": "rf", "bin": index, "bandwidth_hz": 1, "amplitude": 0.001}
            for index in range(100, 165)
        ]
        with self.assertRaisesRegex(RFModelError, "64 active"):
            self.evaluate(model)
        model["inputs"][0]["components"] = []
        self.assertEqual(stream(self.evaluate(model))["components"], [])
        model["stages"][0]["branches"][0]["lo_bin"] = 0
        with self.assertRaises((ValueError, RFModelError)):
            self.evaluate(model)

    def test_mixed_network_extracts_once_and_preserves_fractional_bandwidth(self):
        model = self.mixed_model()
        for component in model["inputs"][0]["components"]:
            component["bandwidth_hz"] = 0.1
        baseline = self.evaluate(model)
        post_network(model, {"s": [[0, 0.5], [0.5, 0]]})
        with patch("rfmodel.coherent_network_file.analyze", wraps=analyze) as extraction:
            result = self.evaluate(model)
        self.assertEqual(extraction.call_count, 1)
        self.assertEqual(len(extraction.call_args.args[1]["frequencies_hz"]), len(powers(baseline)))
        for index, power in powers(baseline).items():
            self.assertAlmostEqual(powers(result, "filtered")[index], 0.25 * power)
        self.assert_history(result)

    def test_recursive_polynomial_aliases_same_root_harmonics(self):
        model = load(ROOT / "examples/coherent-recursive-polynomial.json")
        original = copy.deepcopy(model)
        result = self.evaluate(model)
        self.assertEqual(model, original)
        second = stream(result, "second")
        self.assertEqual(len(second["components"]), 5)
        harmonics = {c["bin"]: c for c in second["components"] if c["kind"] == "harmonic"}
        self.assertAlmostEqual(complex(*harmonics[20]["amplitude"]), 0.0005)
        self.assertAlmostEqual(complex(*harmonics[30]["amplitude"]), 0.0000125)
        self.assertAlmostEqual(complex(*harmonics[40]["amplitude"]), 0.00000015625)
        h2_paths = [o for o in result["stages"][1]["origins"] if o["bin"] == 20]
        self.assertEqual(len(h2_paths), 2)
        self.assertEqual(h2_paths[0]["source_factors"], h2_paths[1]["source_factors"])
        self.assertEqual(h2_paths[0]["coherence_group"], h2_paths[1]["coherence_group"])
        self.assertEqual(h2_paths[0]["source_order"], 2)
        third_order = next(
            o for o in second["origins"] if o["bin"] == 10 and o["kind"] == "intermod"
        )
        self.assertEqual(third_order["source_order"], 3)
        self.assertEqual([f["sign"] for f in third_order["source_factors"]], [-1, 1, 1])
        # A linear third stage must retain the global order, not reset it to one.
        model["stages"].append(
            dict(
                model["stages"][1],
                id="third",
                input="second",
                output="third",
                voltage_coefficients=[0, 1],
            )
        )
        model["outputs"] = ["third"]
        propagated = self.evaluate(model)
        self.assertEqual(stream(propagated, "third")["origins"], second["origins"])

    def test_source_order_cutoff_is_explicit_and_counts_dropped_paths(self):
        model = load(ROOT / "examples/coherent-recursive-polynomial.json")
        for maximum, removed, retained in ((3, 1, 4), (2, 3, 2), (1, 5, 1)):
            model["stages"][1]["max_source_order"] = maximum
            result = self.evaluate(model)
            stage = result["stages"][1]
            self.assertEqual(stage["generated_term_count"], 6)
            self.assertEqual(stage["discarded_term_count"], removed)
            self.assertEqual(len(stream(result, "second")["components"]), retained)
            self.assertTrue(
                all(o["source_order"] <= maximum for o in stream(result, "second")["origins"])
            )

    def test_recursive_polynomial_phase_parity(self):
        model = load(ROOT / "examples/coherent-recursive-polynomial.json")
        positive = stream(self.evaluate(model), "second")
        model["inputs"][0]["components"][0]["amplitude"] = -0.01
        negative = stream(self.evaluate(model), "second")
        for a, b, origin in zip(
            positive["components"], negative["components"], positive["origins"]
        ):
            self.assertAlmostEqual(
                complex(*b["amplitude"]), complex(*a["amplitude"]) * (-1) ** origin["source_order"]
            )
        self.assertEqual(positive["origins"], negative["origins"])

    def test_polynomial_follows_limited_amplifier_and_linear_network(self):
        model = load(ROOT / "examples/coherent-harmonic-combiner.json")
        model["inputs"][1]["components"][0]["amplitude"] = 0.005
        model["stages"].append(
            {
                "id": "poly",
                "type": "polynomial_amplifier",
                "input": "result",
                "output": "poly",
                "voltage_coefficients": [0, 1],
                "max_source_order": 3,
            }
        )
        model["outputs"] = ["poly"]
        result = self.evaluate(model)
        self.assertEqual(stream(result, "poly")["components"], stream(result)["components"])
        self.assertEqual({o["source_order"] for o in stream(result, "poly")["origins"]}, {1, 2, 3})

    def test_recursive_polynomial_matches_independent_two_tone_convolution(self):
        model = load(ROOT / "examples/coherent-recursive-polynomial.json")
        model["sources"].append({"id": "other"})
        model["inputs"][0]["components"][0]["amplitude"] = [0.01, 0.003]
        model["inputs"][0]["components"].append(
            {"source": "other", "bin": 13, "bandwidth_hz": 1, "amplitude": [0.007, -0.002]}
        )
        model["stages"][0]["max_source_order"] = 6
        model["stages"][1].update(voltage_coefficients=[0, 0.8, 0.4, -0.1], max_source_order=6)
        result = self.evaluate(model)
        first = self.library.polynomial_amplifier(
            1e8,
            {10: complex(0.01, 0.003), 13: complex(0.007, -0.002)},
            voltage_coefficients=[0, 1, 0.5],
        )
        first.pop(0, None)
        expected = self.library.polynomial_amplifier(
            1e8, first, voltage_coefficients=[0, 0.8, 0.4, -0.1]
        )
        expected.pop(0, None)
        actual = {}
        for component in stream(result, "second")["components"]:
            actual[component["bin"]] = actual.get(component["bin"], 0j) + complex(
                *component["amplitude"]
            )
        self.assertEqual(set(actual), set(expected))
        for index, wave in expected.items():
            self.assertAlmostEqual(actual[index] / wave, 1 + 0j, places=9)
        self.assertEqual(result["stages"][1]["discarded_term_count"], 0)

    def test_recursive_fractional_bandwidth_is_canonical(self):
        model = load(ROOT / "examples/coherent-recursive-polynomial.json")
        model["inputs"][0]["components"][0]["bandwidth_hz"] = 0.1
        for stage in model["stages"]:
            stage.update(voltage_coefficients=[0, 1, 0.5, -0.2], max_source_order=9)
        output = stream(self.evaluate(model), "second")
        identities = set()
        for origin in output["origins"]:
            factors = tuple(
                (factor["root_id"], factor["sign"]) for factor in origin["source_factors"]
            )
            key = (origin["bin"], origin["kind"], factors)
            self.assertNotIn(key, identities)
            identities.add(key)
            self.assertEqual(origin["bandwidth_hz"], math.fsum([0.1] * origin["source_order"]))

    def test_polynomial_validates_cutoff_and_empty_mixer_bank(self):
        model = load(ROOT / "examples/coherent-recursive-polynomial.json")
        for value in (0, 257, True, 2.5):
            changed = copy.deepcopy(model)
            changed["stages"][0]["max_source_order"] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.evaluate(changed)
        model["stages"].append({"id": "mixer", "type": "ideal_mixer_bank", "branches": []})
        with self.assertRaisesRegex(ValueError, "array size"):
            self.evaluate(model)

    def test_recursive_polynomial_cli_and_failure_preserve_output(self):
        model = load(ROOT / "examples/coherent-recursive-polynomial.json")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "polynomial.json"
            output = Path(directory) / "result.json"
            source.write_text(json.dumps(model), encoding="utf-8")
            env = dict(os.environ) if INSTALLED else dict(os.environ, PYTHONPATH=str(ROOT / "python"))
            command = [sys.executable, "-m", "rfmodel", str(source), "--library", str(LIBRARY_PATH),
                       "--output", str(output)]
            completed = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(json.loads(output.read_text()), self.evaluate(model))
            saved = output.read_bytes()
            model["stages"][1]["max_source_order"] = 0
            source.write_text(json.dumps(model), encoding="utf-8")
            completed = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertNotEqual(completed.returncode, 0)
            self.assertEqual(output.read_bytes(), saved)

    def test_legacy_mixer_coherent_image_addition_is_preserved(self):
        model = {
            "format": "rfmodel.coherent-system",
            "version": 1,
            "spacing_hz": 1e8,
            "sources": [{"id": "rf"}, {"id": "lo"}],
            "inputs": [
                {
                    "id": "input",
                    "components": [
                        {"source": "rf", "bin": 8, "bandwidth_hz": 1, "amplitude": 0.01},
                        {"source": "rf", "bin": 12, "bandwidth_hz": 1, "amplitude": 0.02},
                    ],
                }
            ],
            "stages": [
                {
                    "id": "mix",
                    "type": "ideal_mixer_bank",
                    "branches": [
                        {
                            "id": "out",
                            "input": "input",
                            "lo_source": "lo",
                            "lo_bin": 10,
                            "conversion_gain_db": 0,
                        }
                    ],
                }
            ],
            "outputs": ["out"],
        }
        result = self.evaluate(model)
        self.assertAlmostEqual(powers(result, "out")[2], 0.03**2)
        self.assertNotIn("origin_roots", result)

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

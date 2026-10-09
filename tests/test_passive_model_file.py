"""Native passive device bindings and parameterized JSON network regression."""

import copy
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

LIBRARY_PATH = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[1]
if "--installed" in sys.argv:
    sys.argv.remove("--installed")
else:
    sys.path.insert(0, str(ROOT / "python"))
import rfmodel
from rfmodel import Library, RFModelError
from rfmodel.model_file import analyze, load
from rfmodel.coherent_network_file import analyze_coherent_network


def network(model, ports=2):
    return {
        "format": "rfmodel.linear-network",
        "version": 1,
        "frequencies_hz": [0, 1e9],
        "devices": [{"id": "d", "model": model}],
        "external_ports": [["d", port] for port in range(ports)],
    }


class PassiveModelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def test_native_rlc_equations(self):
        frequency = 1e9
        for reference in (50, 75):
            for element, value, impedance in (
                ("resistor", 50, 50),
                ("inductor", 10e-9, 2j * math.pi * frequency * 10e-9),
                ("capacitor", 2e-12, 1 / (2j * math.pi * frequency * 2e-12)),
            ):
                for connection in ("series", "shunt"):
                    scattering = self.library.ideal_rlc(
                        frequency,
                        element=element,
                        connection=connection,
                        value=value,
                        reference_ohms=reference,
                    )
                    normalized = (
                        impedance / reference if connection == "series" else reference / impedance
                    )
                    sign = 1 if connection == "series" else -1
                    self.assertAlmostEqual(scattering[0][0], sign * normalized / (2 + normalized))
                    self.assertAlmostEqual(scattering[1][0], 2 / (2 + normalized))
                    self.assertEqual(scattering[0][1], scattering[1][0])

    def test_native_rlc_dc_limits(self):
        for element, connection, reflection, transmission in (
            ("inductor", "series", 0, 1),
            ("capacitor", "shunt", 0, 1),
            ("inductor", "shunt", -1, 0),
            ("capacitor", "series", 1, 0),
        ):
            scattering = self.library.ideal_rlc(
                0, element=element, connection=connection, value=1e-9
            )
            self.assertEqual(scattering[0][0], reflection)
            self.assertEqual(scattering[1][0], transmission)

    def test_native_matched_attenuation_and_delay(self):
        for frequency, phase in ((0, 1), (1e9, -1j), (2e9, -1)):
            scattering = self.library.matched_transmission(frequency, loss_db=20, delay_s=0.25e-9)
            self.assertEqual(scattering[0][0], 0)
            self.assertAlmostEqual(scattering[1][0], 0.1 * phase)
            self.assertEqual(scattering[1][0], scattering[0][1])

    def test_native_divider_noise_and_complex_phase(self):
        scattering = self.library.equal_power_divider(1e9, branches=2)
        self.assertAlmostEqual(scattering[1][0], 1 / math.sqrt(2))
        covariance = self.library.passive_noise(scattering)
        thermal = 1.380649e-23 * 290
        self.assertAlmostEqual(covariance[1][1] / thermal, 0.5)
        self.assertAlmostEqual(covariance[1][2] / thermal, -0.5)
        self.assertAlmostEqual(covariance[0][0] / thermal, 0)
        complex_divider = self.library.isolated_power_divider(1e9, [0.5, 0.5j])
        self.assertEqual(complex_divider[0][2], 0.5j)
        self.assertEqual(complex_divider[2][0], 0.5j)
        self.assertEqual(len(self.library.equal_power_divider(0, branches=64)), 65)

    def test_native_quadrature_power_and_isolation(self):
        for fraction in (0, 0.25, 0.5, 1):
            scattering = self.library.quadrature_coupler(1e9, coupled_power_fraction=fraction)
            self.assertEqual(len(scattering), 4)
            self.assertAlmostEqual(scattering[1][0], math.sqrt(1 - fraction))
            self.assertAlmostEqual(scattering[2][0], 1j * math.sqrt(fraction))
            self.assertEqual(scattering[3][0], 0)
            covariance = self.library.passive_noise(scattering)
            self.assertLess(max(abs(value) for row in covariance for value in row), 1e-33)

    def test_rc_network_against_abcd(self):
        document = load(ROOT / "examples/passive-rc-network.json")
        result = analyze(self.library, document)
        for point in result["samples"]:
            frequency = point["frequency_hz"]
            y = (
                1 / 100
                + 2j * math.pi * frequency * document["devices"][-1]["model"]["capacitance_f"]
            )
            denominator = 3 + 100 * y
            self.assertAlmostEqual(complex(*point["s"][1][0]), 2 / denominator)
            self.assertAlmostEqual(complex(*point["s"][0][0]), 1 / denominator)
            scattering = [[complex(*value) for value in row] for row in point["s"]]
            expected_noise = self.library.passive_noise(scattering, 290)
            for row in range(2):
                for column in range(2):
                    self.assertLess(
                        abs(
                            complex(*point["noise_w_per_hz"][row][column])
                            - expected_noise[row][column]
                        ),
                        1e-33,
                    )
            self.assertGreaterEqual(point["noise_analysis"]["noise_figure_db"], 0)

    def test_series_resistor_noise_minimum_boundary_is_explicit(self):
        document = network({"type": "resistor", "connection": "series", "resistance_ohms": 50})
        document["temperature_k"] = 290
        document["noise_analysis"] = {}
        with self.assertRaisesRegex(ValueError, "unit-circle boundary"):
            analyze(self.library, document)
        del document["noise_analysis"]
        self.assertIn("noise_w_per_hz", analyze(self.library, document)["samples"][0])

    def test_json_multiport_models(self):
        cases = (
            ({"type": "equal_power_divider", "branches": 3, "excess_loss_db": 3}, 4),
            ({"type": "isolated_power_divider", "branch_transmissions": [0.5, [0, 0.5]]}, 3),
            ({"type": "quadrature_coupler", "coupled_power_fraction": 0.25}, 4),
            ({"type": "matched_transmission", "loss_db": 20, "delay_s": 0.25e-9}, 2),
        )
        for model, ports in cases:
            document = network(model, ports)
            document["temperature_k"] = 290
            points = analyze(self.library, document)["samples"]
            self.assertEqual(len(points[0]["s"]), ports)
            self.assertIn("noise_w_per_hz", points[1])
        for element, field, value in (
            ("resistor", "resistance_ohms", 50),
            ("inductor", "inductance_h", 1e-9),
            ("capacitor", "capacitance_f", 1e-12),
        ):
            for connection in ("series", "shunt"):
                model = {"type": element, "connection": connection, field: value}
                point = analyze(self.library, network(model))["samples"][1]
                native = self.library.ideal_rlc(
                    1e9, element=element, connection=connection, value=value
                )
                self.assertEqual(complex(*point["s"][1][0]), native[1][0])

    def test_coherent_network_reuses_passive_models(self):
        original = load(ROOT / "examples/coherent-split-delay.json")
        baseline = analyze_coherent_network(self.library, original)
        document = copy.deepcopy(original)
        for device in document["network"]["devices"]:
            if device["id"] in ("split", "combine"):
                del device["s"]
                device["model"] = {"type": "equal_power_divider", "branches": 2}
            else:
                device["model"] = {"type": "matched_transmission", "delay_s": 0.5e-9}
        changed = analyze_coherent_network(self.library, document)
        self.assertAlmostEqual(changed["total_power_w"], baseline["total_power_w"])
        self.assertEqual(len(changed["components"]), len(baseline["components"]))

    def test_native_invalid_parameters(self):
        calls = [
            lambda: self.library.ideal_rlc(1, element="resistor", connection="series", value=0),
            lambda: self.library.ideal_rlc(-1, element="inductor", connection="shunt", value=1),
            lambda: self.library.ideal_rlc(1, element="unknown", connection="series", value=1),
            lambda: self.library.matched_transmission(1, loss_db=-1),
            lambda: self.library.matched_transmission(1, delay_s=-1),
            lambda: self.library.equal_power_divider(1, branches=True),
            lambda: self.library.equal_power_divider(1, branches=65),
            lambda: self.library.isolated_power_divider(1, [1, 1]),
            lambda: self.library.isolated_power_divider(1, [0.5, math.nan]),
            lambda: self.library.quadrature_coupler(1, coupled_power_fraction=1.1),
            lambda: self.library.quadrature_coupler(
                1, coupled_power_fraction=0.5, reference_ohms=0
            ),
        ]
        for call in calls:
            with self.assertRaises((ValueError, TypeError, RFModelError)):
                call()

    def test_json_rejects_ambiguous_units_and_invalid_values(self):
        models = [
            {"type": "resistor", "connection": "series", "value": 50},
            {"type": "resistor", "connection": "series", "resistance_ohms": True},
            {"type": "capacitor", "connection": "parallel", "capacitance_f": 1e-12},
            {"type": "inductor", "connection": "series", "inductance_h": -1},
            {"type": "equal_power_divider", "branches": 2.0},
            {"type": "equal_power_divider", "branches": True},
            {"type": "isolated_power_divider", "branch_transmissions": None},
            {"type": "isolated_power_divider", "branch_transmissions": [1, 1]},
            {"type": "quadrature_coupler", "coupled_power_fraction": 0.5, "phase": 90},
            {"type": "matched_transmission", "delay_s": None},
        ]
        for model in models:
            with self.subTest(model=model), self.assertRaises((ValueError, RFModelError)):
                analyze(self.library, network(model))

    def test_cli_and_failure_preserves_output(self):
        document = load(ROOT / "examples/passive-rc-network.json")
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(Path(rfmodel.__file__).resolve().parents[1])
        with tempfile.TemporaryDirectory() as directory:
            model, output = Path(directory) / "model.json", Path(directory) / "result.json"
            model.write_text(json.dumps(document), encoding="utf-8")
            command = [
                sys.executable,
                "-m",
                "rfmodel",
                str(model),
                "--library",
                LIBRARY_PATH,
                "--output",
                str(output),
            ]
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            saved = output.read_bytes()
            self.assertEqual(len(json.loads(saved)["samples"]), 3)
            document["devices"][0]["model"]["resistance_ohms"] = -50
            model.write_text(json.dumps(document), encoding="utf-8")
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()

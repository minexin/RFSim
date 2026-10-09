"""Incident/outgoing C/P, thermal power flow and cross-wave orientation."""

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
from rfmodel.conversion_network_file import analyze_conversion_network
from rfmodel.model_file import load

K = 1.380649e-23


class LoadedConversionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def test_conjugate_feedback_against_complex_scalar_inverse(self):
        a = 0.2 + 0.1j
        b = 0.05 - 0.02j
        gamma = 0.3 - 0.1j
        ns = 2.0
        ps = 0.4 + 0.2j
        nc = 0.7
        pc = 0.1 - 0.03j
        device = {
            "channels": [(0, 1)],
            "direct": [[a]],
            "conjugate": [[b]],
            "reflection": [gamma],
            "source_covariance": [[ns]],
            "source_complementary": [[ps]],
            "intrinsic_covariance": [[nc]],
            "intrinsic_complementary": [[pc]],
        }
        result = self.library.conversion_network(1e9, [device], loaded_noise=True)

        def basis(c, p):
            xx = (c + p.real) / 2
            yy = (c - p.real) / 2
            xy = p.imag / 2
            return [math.sqrt(xx) + 1j * xy / math.sqrt(xx), 1j * math.sqrt(yy - xy * xy / xx)]

        h = 1 - gamma * a
        q = -gamma * b
        determinant = abs(h) ** 2 - abs(q) ** 2
        incident = []
        outgoing = []
        for e, c in [(u, 0) for u in basis(ns, ps)] + [(0, u) for u in basis(nc, pc)]:
            drive = e + gamma * c
            av = (h.conjugate() * drive - q * drive.conjugate()) / determinant
            bv = a * av + b * av.conjugate() + c
            incident.append(av)
            outgoing.append(bv)
        for actual, values in (
            (result.incident_noise_covariance, incident),
            (result.noise_covariance, outgoing),
        ):
            self.assertAlmostEqual(actual[0][0], sum(abs(v) ** 2 for v in values))
        self.assertAlmostEqual(
            result.incident_noise_complementary[0][0], sum(v * v for v in incident)
        )
        self.assertAlmostEqual(result.noise_complementary[0][0], sum(v * v for v in outgoing))
        self.assertAlmostEqual(
            result.incident_outgoing_noise_covariance[0][0],
            sum(x * y.conjugate() for x, y in zip(incident, outgoing)),
        )
        self.assertAlmostEqual(
            result.incident_outgoing_noise_complementary[0][0],
            sum(x * y for x, y in zip(incident, outgoing)),
        )

    def test_ordinary_limit_matches_loaded_noise(self):
        scattering = [[0.1, 0.7], [0.7, -0.2]]
        intrinsic = self.library.passive_noise(scattering, 290)
        gamma = [0.2 + 0.3j, -0.1 + 0.25j]
        source = [[K * 400 * (1 - abs(gamma[0]) ** 2), 0], [0, K * 200 * (1 - abs(gamma[1]) ** 2)]]
        expected = self.library.loaded_noise(scattering, intrinsic, gamma, source)
        device = {
            "channels": [(0, 1), (1, 1)],
            "direct": scattering,
            "reflection": gamma,
            "intrinsic_covariance": intrinsic,
            "source_covariance": source,
        }
        result = self.library.conversion_network(1e9, [device], loaded_noise=True)
        for i in range(2):
            for j in range(2):
                self.assertAlmostEqual(
                    result.incident_noise_covariance[i][j] / K, expected[0][i][j] / K
                )
                self.assertAlmostEqual(result.noise_covariance[i][j] / K, expected[1][i][j] / K)
                self.assertLess(abs(result.incident_noise_complementary[i][j]), 1e-32)
            self.assertAlmostEqual(result.net_noise_into_device_w_per_hz[i] / K, expected[2][i] / K)

    def test_thermal_equilibrium_and_flow_direction(self):
        base = load(ROOT / "examples/conversion-thermal-load.json")
        s = 0.5 + 0.2j
        gamma = 0.3 - 0.1j
        for temperature in (0, 145, 290, 580):
            document = copy.deepcopy(base)
            document["boundaries"][0]["noise_temperature_k"] = temperature
            result = analyze_conversion_network(self.library, document)
            expected = (
                (temperature - 290)
                * (1 - abs(s) ** 2)
                * (1 - abs(gamma) ** 2)
                / abs(1 - s * gamma) ** 2
            )
            self.assertAlmostEqual(
                result["channels"][0]["net_noise_into_device_w_per_hz"] / K, expected
            )

    def test_wires_preserve_cross_statistics_and_net_flow(self):
        document = load(ROOT / "examples/filtered-conversion-network.json")
        document["loaded_noise"] = True
        result = analyze_conversion_network(self.library, document)
        labels = {(c["device"], c["port"], c["bin"]): i for i, c in enumerate(result["channels"])}
        for index in (8, 12):
            left = labels[("rf_filter", 1, index)]
            right = labels[("mixer", 0, index)]
            self.assertAlmostEqual(
                result["channels"][left]["net_noise_into_device_w_per_hz"] / 1e-20,
                -result["channels"][right]["net_noise_into_device_w_per_hz"] / 1e-20,
            )
            for j in range(len(labels)):
                for field in ("covariance", "complementary"):
                    cross = complex(
                        *result["incident_outgoing_noise_" + field + "_w_per_hz"][left][j]
                    )
                    outgoing = complex(*result["noise_" + field + "_w_per_hz"][right][j])
                    self.assertAlmostEqual(cross / 1e-20, outgoing / 1e-20)

    def test_folded_mixer_c_and_p_cross_orientation(self):
        channels = [(0, 8), (1, 2), (1, 18)]
        a, b = self.library.ideal_mixer_conversion(1, channels, lo_bin=10)
        gamma = 0.3j
        device = {
            "channels": channels,
            "direct": a,
            "conjugate": b,
            "reflection": [0, gamma, 0],
            "source_covariance": [[2, 0, 0], [0, 3, 0], [0, 0, 0]],
        }
        result = self.library.conversion_network(1, [device], loaded_noise=True)
        self.assertAlmostEqual(result.incident_noise_covariance[1][1], 3 + 2 * abs(gamma) ** 2)
        self.assertAlmostEqual(result.incident_outgoing_noise_covariance[1][1], 2 * gamma)
        self.assertAlmostEqual(result.incident_outgoing_noise_covariance[0][1], 0)
        self.assertAlmostEqual(result.incident_outgoing_noise_complementary[0][1], 2)
        self.assertAlmostEqual(result.incident_outgoing_noise_covariance[0][2], 2)
        self.assertAlmostEqual(result.incident_outgoing_noise_complementary[1][2], 2 * gamma)

    def test_dc_thermal_boundary_uses_real_statistics(self):
        document = {
            "format": "rfmodel.conversion-network",
            "version": 1,
            "spacing_hz": 1,
            "loaded_noise": True,
            "devices": [
                {
                    "id": "dc",
                    "bins": [0],
                    "model": {"type": "linear", "s": [[0.5]]},
                    "noise": {"temperature_k": 290},
                }
            ],
            "boundaries": [{"channel": ["dc", 0, 0], "noise_temperature_k": 290}],
        }
        result = analyze_conversion_network(self.library, document)
        for prefix in ("incident_noise_", "incident_outgoing_noise_", "noise_"):
            self.assertEqual(
                result[prefix + "covariance_w_per_hz"], result[prefix + "complementary_w_per_hz"]
            )
        self.assertAlmostEqual(result["channels"][0]["net_noise_into_device_w_per_hz"] / K, 0)

    def test_requested_statistics_preserve_ordinary_outputs(self):
        device = {
            "channels": [(0, 1)],
            "direct": [[0.2]],
            "reflection": [0.3],
            "source": [1],
            "source_covariance": [[2]],
        }
        ordinary = self.library.conversion_network(1, [device])
        loaded = self.library.conversion_network(1, [device], loaded_noise=True)
        self.assertEqual(tuple(ordinary), tuple(loaded)[: len(ordinary)])
        self.assertFalse(hasattr(ordinary, "incident_noise_covariance"))
        with self.assertRaises(TypeError):
            self.library.conversion_network(1, [device], loaded_noise=1)

    def test_thermal_boundary_rejects_conflicting_or_active_input(self):
        base = load(ROOT / "examples/conversion-thermal-load.json")
        for change in (
            {"noise_w_per_hz": 0},
            {"noise_temperature_k": -1},
            {"noise_temperature_k": True},
            {"reflection": 1.01},
            {"noise_temperature_k": math.inf},
        ):
            document = copy.deepcopy(base)
            document["boundaries"][0].update(change)
            with self.assertRaises((ValueError, RFModelError)):
                analyze_conversion_network(self.library, document)
        document = copy.deepcopy(base)
        document["loaded_noise"] = 1
        with self.assertRaises(ValueError):
            analyze_conversion_network(self.library, document)

    def test_cli_preserves_result_when_extended_output_fails(self):
        document = load(ROOT / "examples/conversion-thermal-load.json")
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(Path(rfmodel.__file__).resolve().parents[1])
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.json"
            output = Path(directory) / "output.json"
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
            model.write_text(json.dumps(document), encoding="utf-8")
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            original = output.read_bytes()
            document["boundaries"][0]["reflection"] = 2
            model.write_text(json.dumps(document), encoding="utf-8")
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(output.read_bytes(), original)


if __name__ == "__main__":
    unittest.main()

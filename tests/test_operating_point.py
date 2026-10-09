"""Affine waves, unchanged noise and operating-point consistency in physical networks."""

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
from rfmodel.conversion_file import analyze_conversion


class OperatingPointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def example(self, feedback=False):
        name = "nonlinear-mixer-feedback" if feedback else "nonlinear-mixer-network"
        return json.loads((ROOT / ("examples/" + name + ".json")).read_text())

    def row(self, result, name, port, bin_index):
        return next(
            (i, row)
            for i, row in enumerate(result["channels"])
            if (row["device"], row["port"], row["bin"]) == (name, port, bin_index)
        )

    def close(self, actual, expected, scale=1):
        self.assertLessEqual(abs(actual - expected), 4e-9 * max(scale, abs(expected)))

    def test_fixed_product_fourier_and_complex_derivatives(self):
        channels = [(0, 0), (0, 4), (1, 0), (1, 3), (1, 5)] + [
            (2, b) for b in [0, 1, 3, 4, 5, 7, 9]
        ]
        waves = [0.2, 1 + 0.3j, 0.1, 2 - 0.4j, 0.5 + 0.7j] + [0] * 7

        def evaluate(values):
            return self.library.linearize_bilinear_mixer(
                1, channels, values, lo_reference_amplitude=2
            )

        model = evaluate(waves)
        spectra = [{}, {}]
        for (port, b), v in zip(channels, waves):
            if port == 2:
                continue
            spectra[port][b] = v / math.sqrt(2) if b else v
            if b:
                spectra[port][-b] = complex(v).conjugate() / math.sqrt(2)
        expected = {}
        for r, x in spectra[0].items():
            for l, y in spectra[1].items():
                expected[r + l] = expected.get(r + l, 0) + math.sqrt(2) * 0.5 * x * y
        for i, (port, b) in enumerate(channels):
            if port == 2:
                self.close(model.operating_outgoing[i], expected[b] * (math.sqrt(2) if b else 1))
        for j in range(5):
            for direction in [1, 1j]:
                if channels[j][1] == 0 and direction == 1j:
                    continue
                hi, lo = waves.copy(), waves.copy()
                hi[j] += direction * 1e-6
                lo[j] -= direction * 1e-6
                derivative = [
                    (h - l) / 2e-6
                    for h, l in zip(
                        evaluate(hi).operating_outgoing, evaluate(lo).operating_outgoing
                    )
                ]
                for i, v in enumerate(derivative):
                    self.close(
                        v,
                        model.direct[i][j] * direction
                        + model.conjugate[i][j] * complex(direction).conjugate(),
                    )

    def test_lo_scaling_zero_and_full_closure(self):
        channels = [(0, 12), (1, 10), (2, 2), (2, 22)]
        for lo in [0, 0.5, 2, 3j]:
            m = self.library.linearize_bilinear_mixer(
                1, channels, [1, lo, 0, 0], lo_reference_amplitude=2
            )
            self.close(m.operating_outgoing[2], complex(lo).conjugate() / 2)
            self.close(m.operating_outgoing[3], lo / 2)
        with self.assertRaises(RFModelError):
            self.library.linearize_bilinear_mixer(
                1, channels[:-1], [0, 0, 0], lo_reference_amplitude=1
            )
        for reference in [0, -1, math.inf, math.nan]:
            with self.assertRaises(RFModelError):
                self.library.linearize_bilinear_mixer(
                    1, channels, [1, 1, 0, 0], lo_reference_amplitude=reference
                )

    def test_physical_feedback_analytic_root_and_true_equations(self):
        result = analyze_conversion_network(self.library, self.example(True))
        b = (30 - math.sqrt(500)) / 2
        self.close(complex(*result["channels"][2]["outgoing"]), b)
        a = [complex(*row["incident"]) for row in result["channels"]]
        out = [complex(*row["outgoing"]) for row in result["channels"]]
        self.close(out[2], a[0] * a[1])
        for left, right in [(0, 4), (1, 5), (2, 3)]:
            self.close(a[left], out[right])
            self.close(a[right], out[left])
        self.assertGreater(result["operating_point"]["iterations"], 1)
        self.assertLessEqual(result["operating_point"]["scaled_residual"], 1)

    def test_rf_nominal_and_shared_noise_after_solve(self):
        result = analyze_conversion_network(self.library, self.example())
        self.assertEqual(result["wave_relation"], "nonlinear_operating_point")
        for b in [2, 22]:
            _, row = self.row(result, "if_filter", 1, b)
            self.close(complex(*row["outgoing"]), 0.125)
        for b, noise in [(1, 0), (3, 0), (21, 6.25e-12), (23, 6.25e-12)]:
            i, _ = self.row(result, "if_filter", 1, b)
            self.close(complex(*result["noise_covariance_w_per_hz"][i][i]), noise, 1e-10)

    def test_actual_lo_drive_changes_nominal_gain(self):
        document = self.example()
        for boundary in document["boundaries"]:
            if boundary["channel"] == ["mixer", 1, 10]:
                boundary["source"] = 0.5
        result = analyze_conversion_network(self.library, document)
        _, row = self.row(result, "if_filter", 1, 2)
        self.close(complex(*row["outgoing"]), 0.0625)
        i, _ = self.row(result, "if_filter", 1, 21)
        self.close(complex(*result["noise_covariance_w_per_hz"][i][i]), 1.5625e-12, 1e-10)

    def test_converged_jacobian_is_used_for_reference_noise_figure(self):
        document = self.example()
        document["noise_analyses"] = [
            dict(
                name="desired",
                reference_channels=[["rf_filter", 0, 12]],
                thermal_channels=[["rf_filter", 0, 12]],
                output_channel=["if_filter", 1, 2],
            )
        ]
        result = analyze_conversion_network(self.library, document)
        metric = result["noise_analyses"][0]
        self.close(metric["reference_gain"], 0.125**2)
        self.close(metric["noise_factor"], 1)

    def test_warm_start_and_different_solution_branch(self):
        document = self.example(True)
        result = analyze_conversion_network(self.library, document)
        document["operating_point"]["initial_incident"] = [
            row["incident"] for row in result["channels"]
        ]
        self.assertEqual(
            analyze_conversion_network(self.library, document)["operating_point"]["iterations"], 0
        )
        b = (30 + math.sqrt(500)) / 2
        document["operating_point"]["initial_incident"] = [1 + 0.1 * b, 2 + 0.2 * b, 0, b, 0, 0]
        upper = analyze_conversion_network(self.library, document)
        self.close(complex(*upper["channels"][2]["outgoing"]), b)

    def test_loaded_and_additional_noise_linear_limit(self):
        devices = [
            dict(
                channels=[(0, 1)],
                direct=[[0.5]],
                source=[1],
                reflection=[0.2],
                source_covariance=[[1e-9]],
            )
        ]
        options = dict(
            output_offset=[0.3], loaded_noise=True, additional_source_covariance=[[2e-9]]
        )
        baseline = self.library.conversion_network(1, devices, **options)
        actual = self.library.solve_conversion_operating_point(1, devices, **options)
        self.close(actual.waves.outgoing[0], baseline.outgoing[0])
        for field in [
            "noise_covariance",
            "noise_complementary",
            "incident_noise_covariance",
            "incident_outgoing_noise_covariance",
            "net_noise_into_device_w_per_hz",
        ]:
            self.assertEqual(getattr(actual.waves, field), getattr(baseline, field))

    def test_direct_python_network_and_duplicate_model_rejection(self):
        channels = [(0, 0), (1, 0), (2, 0)]
        devices = [dict(channels=channels, direct=[[0] * 3 for _ in range(3)], source=[1, 2, 0])]
        spec = dict(device=0, lo_reference_amplitude=1, gain_db=-10 * math.log10(2))
        result = self.library.solve_conversion_operating_point(
            1, devices, mixers=[spec], initial_incident=[0] * 3
        )
        self.close(result.waves.outgoing[2], 2)
        with self.assertRaises(RFModelError):
            self.library.solve_conversion_operating_point(1, devices, mixers=[spec, spec])
        for spec in [
            dict(device=True, lo_reference_amplitude=1),
            dict(device=0, lo_reference_amplitude=True),
            dict(device=0, lo_reference_amplitude=1, extra=0),
        ]:
            with self.assertRaises((TypeError, ValueError, RFModelError)):
                self.library.solve_conversion_operating_point(1, devices, mixers=[spec])

    def test_limits_and_invalid_options(self):
        for options in [
            dict(max_iterations=0),
            dict(max_iterations=201),
            dict(max_iterations=True),
            dict(max_backtracks=41),
            dict(relative_tolerance=0),
            dict(relative_tolerance=True),
            dict(absolute_tolerance=-1),
            dict(initial_incident=[0]),
            dict(extra=1),
        ]:
            d = self.example(True)
            d["operating_point"] = options
            with self.assertRaises((TypeError, ValueError, RFModelError)):
                analyze_conversion_network(self.library, d)
        d = self.example(True)
        d["operating_point"] = {"max_iterations": 1}
        with self.assertRaisesRegex(RFModelError, "iteration limit"):
            analyze_conversion_network(self.library, d)

    def test_strict_json_model_and_explicit_opt_in(self):
        mutations = [
            lambda d: d.pop("operating_point"),
            lambda d: d["devices"][0]["model"].update(lo_reference_amplitude=False),
            lambda d: d["devices"][0]["model"].update(rf_port=True),
            lambda d: d["devices"][0]["model"].update(operating_incident=[0] * 3),
            lambda d: d["operating_point"].update(initial_incident=[False] * 6),
        ]
        for mutation in mutations:
            d = self.example(True)
            mutation(d)
            with self.assertRaises((ValueError, RFModelError)):
                analyze_conversion_network(self.library, d)

    def test_reflected_if_is_solved_without_supplied_point(self):
        d = self.example()
        for boundary in d["boundaries"]:
            if boundary["channel"] in [["if_filter", 1, 2], ["if_filter", 1, 22]]:
                boundary["reflection"] = 0.2
        result = analyze_conversion_network(self.library, d)
        for b in [2, 22]:
            _, row = self.row(result, "mixer", 2, b)
            self.close(complex(*row["incident"]), 0.00625)

    def test_cli_nonconvergence_preserves_existing_output(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / "input.json", Path(folder) / "output.json"
            document = self.example(True)
            source.write_text(json.dumps(document))
            env = dict(os.environ, PYTHONPATH=str(Path(rfmodel.__file__).parent.parent))
            command = [
                sys.executable,
                "-m",
                "rfmodel",
                str(source),
                "--library",
                LIBRARY_PATH,
                "--output",
                str(output),
            ]
            run = subprocess.run(command, capture_output=True, text=True, env=env)
            self.assertEqual(run.returncode, 0, run.stderr)
            saved = output.read_bytes()
            document["operating_point"]["max_iterations"] = 1
            source.write_text(json.dumps(document))
            run = subprocess.run(command, capture_output=True, text=True, env=env)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn("iteration limit", run.stderr)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()

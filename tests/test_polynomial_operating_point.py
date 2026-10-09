"""Polynomial ABI, full support, true network waves and converged noise regressions."""

import cmath
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


def sampled(bins, waves, coefficients, output_bins, reference=50):
    """Independent real time-domain powers and Fourier projection; no aliasing."""
    count = 512
    result = {b: 0j for b in output_bins}
    for i in range(count):
        phase = 2 * math.pi * i / count
        voltage = sum(
            math.sqrt(reference) * (math.sqrt(2) if b else 1) * (v * cmath.exp(1j * b * phase)).real
            for b, v in zip(bins, waves)
        )
        value = sum(c * voltage**n for n, c in enumerate(coefficients))
        for b in output_bins:
            result[b] += (
                value
                * cmath.exp(-1j * b * phase)
                / count
                * (math.sqrt(2) if b else 1)
                / math.sqrt(reference)
            )
    return result


class PolynomialOperatingPointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def close(self, actual, expected, scale=1):
        self.assertLessEqual(abs(actual - expected), 2e-8 * max(scale, abs(expected)))

    def example(self, name="nonlinear-polynomial-feedback"):
        return json.loads((ROOT / "examples" / (name + ".json")).read_text())

    def local(self, channels, waves, coefficients, **options):
        return self.library.linearize_polynomial_amplifier(
            1e6, channels, waves, voltage_coefficients=coefficients, **options
        )

    def test_structural_frequency_planning(self):
        for bins, coefficients, expected in [
            ([1], [0, 0, 1], (0, 2)),
            ([1], [0, 0, 0, 1], (1, 3)),
            ([0, 2], [0, 0, 0, 1], (0, 2, 4, 6)),
            ([1], [0] * 11 + [1], (1, 3, 5, 7, 9, 11)),
            ([1], [0], ()),
            ([1], [2], (0,)),
        ]:
            self.assertEqual(self.library.polynomial_output_bins(bins, coefficients), expected)
        for bins in [[], [True], [-1], [1, 1], [2**31]]:
            with self.assertRaises((ValueError, RFModelError)):
                self.library.polynomial_output_bins(bins, [0, 1])
        with self.assertRaises(RFModelError):
            self.library.polynomial_output_bins([2**31 - 1], [0, 0, 1])

    def test_time_domain_nominal_and_complex_derivatives(self):
        for degree in [2, 3, 5, 11]:
            coefficients = [0.01, 1.3] + [(-1) ** n * 0.02 / n for n in range(2, degree + 1)]
            output_bins = self.library.polynomial_output_bins([0, 2, 5], coefficients)
            channels = [(0, 0), (0, 2), (0, 5)] + [(1, b) for b in output_bins]
            waves = [0.02, 0.035 + 0.01j, 0.018 - 0.006j] + [0] * len(output_bins)
            point = self.local(channels, waves, coefficients)
            expected = sampled([0, 2, 5], waves[:3], coefficients, output_bins)
            for i, b in enumerate(output_bins, 3):
                self.close(point.operating_outgoing[i], expected[b])
            for column in range(3):
                for direction in ([1] if column == 0 else [1, 1j]):
                    hi, lo = waves[:3], waves[:3]
                    hi[column] += 1e-6 * direction
                    lo[column] -= 1e-6 * direction
                    upper = sampled([0, 2, 5], hi, coefficients, output_bins)
                    lower = sampled([0, 2, 5], lo, coefficients, output_bins)
                    for i, b in enumerate(output_bins, 3):
                        derivative = (
                            point.direct[i][column] * direction
                            + point.conjugate[i][column] * complex(direction).conjugate()
                        )
                        self.close(derivative, (upper[b] - lower[b]) / 2e-6)

    def test_general_offset_with_constant_and_mixed_orders(self):
        channels = [(0, 0), (0, 1), (1, 0), (1, 1), (1, 2), (1, 3)]
        waves = [0.02, 0.1 + 0.03j, 0, 0, 0, 0]
        point = self.local(channels, waves, [0.1, 2, 0.3, -0.02])
        result = self.library.conversion_network(
            1e6,
            [dict(channels=channels, direct=point.direct, conjugate=point.conjugate, source=waves)],
            output_offset=point.output_offset,
        )
        for actual, expected in zip(result.outgoing, point.operating_outgoing):
            self.close(actual, expected)
        self.assertNotAlmostEqual(point.output_offset[2], -point.operating_outgoing[2])

    def test_feedback_root_harmonic_and_correlated_noise(self):
        result = analyze_conversion_network(self.library, self.example())
        low, high = 0.0, 0.2
        for _ in range(80):
            a = (low + high) / 2
            if a + 0.2 * (2 * a - 1.5 * a**3) > 0.2:
                high = a
            else:
                low = a
        a = (low + high) / 2
        self.close(complex(*result["channels"][0]["incident"]), a)
        self.close(complex(*result["channels"][1]["outgoing"]), 2 * a - 1.5 * a**3)
        self.close(complex(*result["channels"][2]["outgoing"]), -0.5 * a**3)
        radial, tangent, harmonic = 2 - 4.5 * a * a, 2 - 1.5 * a * a, -1.5 * a * a
        rr, ri = radial / (1 + 0.2 * radial), tangent / (1 + 0.2 * tangent)
        hr, hi = harmonic / (1 + 0.2 * radial), harmonic / (1 + 0.2 * tangent)
        self.close(
            complex(*result["noise_covariance_w_per_hz"][1][2]),
            0.5 * (rr * hr + ri * hi) * 1e-9,
            1e-9,
        )
        self.close(
            complex(*result["noise_complementary_w_per_hz"][1][2]),
            0.5 * (rr * hr - ri * hi) * 1e-9,
            1e-9,
        )
        self.assertGreater(result["operating_point"]["iterations"], 1)

    def test_three_model_types_in_one_network(self):
        result = analyze_conversion_network(
            self.library, self.example("nonlinear-mixed-amplifiers")
        )
        bins = (2, 6, 18, 22, 26, 42, 46, 66)
        values = sampled([2, 22], [0.02, 0.02], [0, 2, 0, -0.02], bins)
        r = math.sqrt(sum(abs(v) ** 2 for v in values.values()))
        anchor = math.sqrt(10 ** (-2.9))
        headroom = math.sqrt(10 ** (-0.7)) - math.sqrt(0.1)
        output = math.sqrt(0.1) + headroom * math.tanh(
            10 * (3 * 10 ** (-0.05) - 2) * (r - anchor) / headroom
        )
        gain = output / r
        for row in result["channels"]:
            if row["device"] == "compressor" and row["port"] == 1:
                self.close(complex(*row["outgoing"]), values[row["bin"]] * gain)
        self.assertLessEqual(result["operating_point"]["scaled_residual"], 1)

    def test_unoccupied_input_produces_noise_and_complementary_correlation(self):
        coefficients = [0, 0, 0, 0.1]
        bins = self.library.polynomial_output_bins([2, 5], coefficients)
        channels = [(0, 2), (0, 5)] + [(1, b) for b in bins]
        n = len(channels)
        pump = 0.02 + 0.03j
        noise = [[0] * n for _ in range(n)]
        noise[0][0] = 1e-9
        result = self.library.solve_conversion_operating_point(
            1e6,
            [
                dict(
                    channels=channels,
                    direct=[[0] * n for _ in range(n)],
                    source=[0, pump] + [0] * (n - 2),
                    source_covariance=noise,
                )
            ],
            polynomials=[dict(device=0, voltage_coefficients=coefficients)],
        ).waves
        hi, lo = channels.index((1, 12)), channels.index((1, 8))
        coupling = 1.5 * 0.1 * 50 * pump * pump
        self.close(result.outgoing[hi], 0)
        self.close(result.noise_covariance[hi][hi], abs(coupling) ** 2 * 1e-9, 1e-12)
        self.close(result.noise_covariance[hi][lo], 0, 1e-12)
        self.close(result.noise_complementary[hi][lo], coupling**2 * 1e-9, 1e-12)

    def test_rectified_dc_and_harmonic_noise(self):
        a = 0.03 + 0.04j
        device = dict(
            channels=[(0, 1), (1, 0), (1, 2)],
            direct=[[0] * 3 for _ in range(3)],
            source=[a, 0, 0],
            source_covariance=[[1e-9, 0, 0], [0, 0, 0], [0, 0, 0]],
        )
        result = self.library.solve_conversion_operating_point(
            1,
            [device],
            polynomials=[dict(device=0, voltage_coefficients=[0, 0, 0.4])],
            loaded_noise=True,
        ).waves
        self.close(result.outgoing[1], 0.4 * math.sqrt(50) * abs(a) ** 2)
        density = 2 * 0.4**2 * 50 * abs(a) ** 2 * 1e-9
        self.close(result.noise_covariance[1][1], density, 1e-9)
        self.close(result.noise_complementary[1][1], density, 1e-9)
        self.close(result.noise_covariance[2][2], density, 1e-9)
        self.close(result.noise_complementary[2][2], 0, 1e-9)

    def test_converged_reference_noise_figure(self):
        doc = self.example()
        doc["devices"] = doc["devices"][:1]
        doc["connections"] = []
        doc["boundaries"] = [
            dict(channel=["polynomial", 0, 1], source=0.1),
            dict(channel=["polynomial", 1, 1]),
            dict(channel=["polynomial", 1, 3]),
        ]
        doc["noise_analyses"] = [
            dict(
                name="nf",
                reference_channels=[["polynomial", 0, 1]],
                thermal_channels=[["polynomial", 0, 1]],
                output_channel=["polynomial", 1, 1],
            )
        ]
        metric = analyze_conversion_network(self.library, doc)["noise_analyses"][0]
        self.close(metric["reference_gain"], 1.97**2 + 0.015**2)
        self.close(metric["noise_factor"], 1)

    def test_warm_start(self):
        doc = self.example()
        initial = analyze_conversion_network(self.library, doc)
        doc["operating_point"]["initial_incident"] = [
            row["incident"] for row in initial["channels"]
        ]
        self.assertEqual(
            analyze_conversion_network(self.library, doc)["operating_point"]["iterations"], 0
        )

    def test_custom_ports_reference_and_constant_limit(self):
        point = self.local(
            [(7, 1), (3, 0), (7, 0), (3, 1)],
            [0.2, 0.1, 0, 0.3j],
            [0.4, 2],
            input_port=3,
            output_port=7,
            reference_ohms=4,
        )
        self.close(point.operating_outgoing[0], 0.6j)
        self.close(point.operating_outgoing[2], 0.4)
        self.close(point.direct[2][1] + point.conjugate[2][1], 2)

    def test_missing_support_fails_even_at_zero_drive(self):
        with self.assertRaisesRegex(RFModelError, "missing polynomial output"):
            self.local([(0, 1), (1, 1)], [0, 0], [0, 2, 0, -0.02])
        with self.assertRaisesRegex(RFModelError, "complex DC"):
            self.local([(0, 0), (1, 0)], [1j, 0], [0, 1])

    def test_strict_coefficients_and_options(self):
        for coefficients in [[], [0] * 13, [False], ["1"], [math.inf], [math.nan], {0: 1}]:
            with self.assertRaises((ValueError, TypeError, RFModelError)):
                self.library.polynomial_output_bins([1], coefficients)
        for fields in [
            dict(voltage_coefficients=[False]),
            dict(voltage_coefficients="0,1"),
            dict(input_port=True),
            dict(extra=1),
        ]:
            doc = self.example()
            doc["devices"][0]["model"].update(fields)
            with self.assertRaises((ValueError, RFModelError)):
                analyze_conversion_network(self.library, doc)
        doc = self.example()
        doc.pop("operating_point")
        with self.assertRaises(ValueError):
            analyze_conversion_network(self.library, doc)

    def test_duplicate_model_indices_and_polynomial_parameter_errors(self):
        device = dict(channels=[(0, 1), (1, 1)], direct=[[0, 0], [0, 0]], source=[0.1, 0])
        for spec in [
            dict(device=True, voltage_coefficients=[0, 1]),
            dict(device=0, voltage_coefficients=[0, 1], input_port=True),
            dict(device=0, voltage_coefficients=[0, 1], extra=1),
        ]:
            with self.assertRaises((TypeError, ValueError, RFModelError)):
                self.library.solve_conversion_operating_point(1, [device], polynomials=[spec])
        with self.assertRaises(RFModelError):
            self.library.solve_conversion_operating_point(
                1, [device, device], polynomials=[dict(device=0, voltage_coefficients=[0, 1])] * 2
            )

    def test_fixed_offset_additional_noise(self):
        device = dict(channels=[(0, 1), (1, 1)], direct=[[0, 0], [0, 0]], source=[0.1, 0])
        result = self.library.solve_conversion_operating_point(
            1,
            [device],
            polynomials=[dict(device=0, voltage_coefficients=[0, 2])],
            output_offset=[0, 0.03j],
            additional_source_covariance=[[1e-9, 0], [0, 0]],
        ).waves
        self.close(result.outgoing[1], 0.2 + 0.03j)
        self.close(result.noise_covariance[1][1], 4e-9, 1e-9)

    def test_cli_failure_preserves_previous_result(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / "input.json", Path(folder) / "output.json"
            doc = self.example()
            source.write_text(json.dumps(doc))
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
            first = subprocess.run(command, capture_output=True, text=True, env=env)
            self.assertEqual(first.returncode, 0, first.stderr)
            saved = output.read_bytes()
            doc["operating_point"]["max_iterations"] = 1
            source.write_text(json.dumps(doc))
            failed = subprocess.run(command, capture_output=True, text=True, env=env)
            self.assertNotEqual(failed.returncode, 0)
            self.assertIn("iteration limit", failed.stderr)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()

"""High-order C/Python/JSON working points with independent response/noise oracles."""

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

PARAMETERS = dict(
    power_gain_db=20,
    output_p1db_dbm=20,
    output_saturation_dbm=23,
    nonlinear_voltage_coefficients=[0, 0.1],
    generating_bins=[1],
)


def scalar(a):
    q, p1 = 10 ** (-0.05), math.sqrt(10 ** (-2.9))
    head, slope = math.sqrt(10 ** (-0.7)) - math.sqrt(0.1), 10 * (3 * q - 2)
    if a <= p1:
        f = 10 * a * (1 - (1 - q) * (a / p1) ** 2)
        df = 10 * (1 - 3 * (1 - q) * (a / p1) ** 2)
    else:
        u = slope * (a - p1) / head
        f = math.sqrt(0.1) + head * math.tanh(u)
        df = slope / math.cosh(u) ** 2
    knee, limit = math.sqrt(10 ** (-3.4)), math.sqrt(10 ** (-2.8))
    limited, dl = a, 1
    if a > knee:
        u = (a - knee) / (limit - knee)
        limited = knee + (limit - knee) * math.tanh(u)
        dl = 1 / math.cosh(u) ** 2
    return f, df, limited, dl


def sampled(bins, waves, coefficients, generating, outputs, reference=50):
    """Real time-domain powers projected onto positive Fourier bins."""
    radius = math.sqrt(sum(abs(a) ** 2 for a in waves))
    fundamental, _, limited, _ = scalar(radius)
    gain, scale = (fundamental / radius, limited / radius) if radius else (10, 1)
    result = {b: gain * sum(a for k, a in zip(bins, waves) if k == b) for b in outputs}
    for i in range(256):
        phase = 2 * math.pi * i / 256
        voltage = (
            math.sqrt(2 * reference)
            * scale
            * sum(
                (a * cmath.exp(1j * b * phase)).real for b, a in zip(bins, waves) if b in generating
            )
        )
        value = sum(c * voltage ** (n + 2) for n, c in enumerate(coefficients))
        for b in outputs:
            result[b] += value * cmath.exp(-1j * b * phase) * math.sqrt(2 / reference) / 256
    return result


class HighorderOperatingPointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def close(self, a, b, scale=1):
        self.assertLessEqual(abs(a - b), 2e-8 * max(scale, abs(b)))

    def example(self):
        return json.loads((ROOT / "examples/nonlinear-highorder-feedback.json").read_text())

    def local(self, channels, waves, **options):
        return self.library.linearize_highorder_amplifier(
            1e6, channels, waves, **dict(PARAMETERS, **options)
        )

    def test_sampled_multitone_response_and_derivatives(self):
        channels = [(0, 1), (0, 3)] + [(1, b) for b in range(1, 34)]
        for order in (2, 3, 5, 11):
            coefficients = [0] * (order - 2) + [0.03]
            for amplitude in (0, 0.01, 0.03, 0.1):
                waves = [amplitude, amplitude * (0.2 + 0.3j)] + [0] * 33
                options = dict(
                    nonlinear_voltage_coefficients=coefficients,
                    generating_bins=[1, 3],
                    reference_ohms=75,
                )
                point = self.local(channels, waves, **options)
                expected = sampled([1, 3], waves[:2], coefficients, [1, 3], range(1, 34), 75)
                for row in range(2, 35):
                    self.close(point.operating_outgoing[row], expected[row - 1])
                for column in (0, 1):
                    for step in (1e-7, 1e-7j):
                        high, low = list(waves), list(waves)
                        high[column] += step
                        low[column] -= step
                        hi = sampled([1, 3], high[:2], coefficients, [1, 3], range(1, 34), 75)
                        lo = sampled([1, 3], low[:2], coefficients, [1, 3], range(1, 34), 75)
                        for row in range(2, 35):
                            expected = (hi[row - 1] - lo[row - 1]) / (2 * abs(step))
                            actual = (
                                point.direct[row][column] * step
                                + point.conjugate[row][column] * step.conjugate()
                            ) / abs(step)
                            self.close(actual, expected)

    def test_affine_offset_and_output_drive(self):
        waves = [0.02 + 0.03j, 0.7j, -0.3]
        point = self.local([(0, 1), (1, 1), (1, 3)], waves)
        for i in range(3):
            self.close(
                point.output_offset[i]
                + sum(
                    point.direct[i][j] * waves[j] + point.conjugate[i][j] * waves[j].conjugate()
                    for j in range(3)
                ),
                point.operating_outgoing[i],
            )
            for j in (1, 2):
                self.assertEqual(point.direct[i][j], 0)
                self.assertEqual(point.conjugate[i][j], 0)
        self.assertNotAlmostEqual(point.output_offset[2], -point.operating_outgoing[2])

    def test_feedback_root_and_correlated_noise(self):
        result = analyze_conversion_network(self.library, self.example())
        lo, hi = 0.0, 0.2
        for _ in range(80):
            a = (lo + hi) / 2
            f, _, l, _ = scalar(a)
            if a + 0.2 * (f + 7.5 * l**3) > 0.2:
                hi = a
            else:
                lo = a
        a = (lo + hi) / 2
        self.close(complex(*result["channels"][0]["incident"]), a)
        f, df, l, dl = scalar(a)
        fundamental, harmonic = f + 7.5 * l**3, 2.5 * l**3
        self.close(complex(*result["channels"][1]["outgoing"]), fundamental)
        self.close(complex(*result["channels"][2]["outgoing"]), harmonic)
        radial, tangent = df + 22.5 * l * l * dl, fundamental / a
        rr, ri = radial / (1 + 0.2 * radial), tangent / (1 + 0.2 * tangent)
        hr, hi = 7.5 * l * l * dl / (1 + 0.2 * radial), 3 * harmonic / a / (1 + 0.2 * tangent)
        for key, sign in [("noise_covariance_w_per_hz", 1), ("noise_complementary_w_per_hz", -1)]:
            self.close(complex(*result[key][1][1]), 0.5 * (rr * rr + sign * ri * ri) * 1e-9, 1e-9)
            self.close(complex(*result[key][2][2]), 0.5 * (hr * hr + sign * hi * hi) * 1e-9, 1e-9)
            self.close(complex(*result[key][1][2]), 0.5 * (rr * hr + sign * ri * hi) * 1e-9, 1e-9)
        self.assertGreater(result["operating_point"]["iterations"], 1)

    def test_propagating_distortion_drives_generated_noise(self):
        channels = [(0, 1), (0, 3), (1, 1), (1, 3)]
        waves = [0.025, 0.025j, 0, 0]
        point = self.local(channels, waves)
        device = dict(
            channels=channels,
            direct=[[0] * 4 for _ in channels],
            source=waves,
            source_covariance=[[1e-9 if i == j == 1 else 0 for j in range(4)] for i in range(4)],
        )
        result = self.library.solve_conversion_operating_point(
            1e6, [device], highorders=[dict(device=0, **PARAMETERS)]
        ).waves
        h = 1e-7
        slopes = []
        for step in (h, 1j * h):
            hi, lo = waves[:2], waves[:2]
            hi[1] += step
            lo[1] -= step
            upper = sampled([1, 3], hi, [0, 0.1], [1], [1, 3])
            lower = sampled([1, 3], lo, [0, 0.1], [1], [1, 3])
            slopes.append([(upper[b] - lower[b]) / (2 * h) for b in (1, 3)])
        for i in range(2):
            for j in range(2):
                dx, dy = slopes
                c = 0.5e-9 * (dx[i] * dx[j].conjugate() + dy[i] * dy[j].conjugate())
                p = 0.5e-9 * (dx[i] * dx[j] + dy[i] * dy[j])
                self.close(result.noise_covariance[i + 2][j + 2], c, 1e-9)
                self.close(result.noise_complementary[i + 2][j + 2], p, 1e-9)
        self.assertGreater(abs(point.conjugate[3][1]), 0)

    def test_empty_coefficients_and_generating_set(self):
        for options in (dict(nonlinear_voltage_coefficients=[]), dict(generating_bins=[])):
            point = self.local([(0, 1), (1, 1)], [0.1, 0], **options)
            self.close(point.operating_outgoing[1], scalar(0.1)[0])

    def test_custom_ports_and_blocked_dc(self):
        point = self.local(
            [(7, 1), (3, 0), (7, 0), (3, 1), (7, 3)],
            [0, 0, 0, 0.01j, 0],
            input_port=3,
            output_port=7,
            reference_ohms=75,
        )
        self.close(point.operating_outgoing[4], -3.75e-6j)
        self.assertEqual(point.operating_outgoing[2], 0)
        with self.assertRaises(RFModelError):
            self.local([(0, 0), (0, 1), (1, 1), (1, 3)], [0.1, 0.01, 0, 0])

    def test_support_required_at_zero_drive(self):
        with self.assertRaises(RFModelError):
            self.local([(0, 1), (1, 1)], [0, 0])

    def test_invalid_parameters(self):
        for fields in [
            dict(generating_bins=[1, 1]),
            dict(generating_bins=[0]),
            dict(generating_bins=[3]),
            dict(generating_bins=[True]),
            dict(generating_bins=[2**32]),
            dict(generating_bins="1"),
            dict(nonlinear_voltage_coefficients=[0] * 11),
            dict(nonlinear_voltage_coefficients=[False]),
            dict(nonlinear_voltage_coefficients=[math.nan]),
            dict(nonlinear_voltage_coefficients="0"),
            dict(output_saturation_dbm=20),
            dict(power_gain_db=True),
            dict(input_port=True),
        ]:
            with (
                self.subTest(fields=fields),
                self.assertRaises((ValueError, TypeError, RFModelError)),
            ):
                self.local([(0, 1), (1, 1), (1, 3)], [0.01, 0, 0], **fields)

    def test_json_strictness(self):
        for fields in [
            dict(extra=1),
            dict(generating_bins=None),
            dict(nonlinear_voltage_coefficients=[False]),
            dict(output_saturation_dbm="23"),
        ]:
            doc = self.example()
            doc["devices"][0]["model"].update(fields)
            with self.assertRaises((ValueError, TypeError, RFModelError)):
                analyze_conversion_network(self.library, doc)
        doc = self.example()
        doc.pop("operating_point")
        with self.assertRaises(ValueError):
            analyze_conversion_network(self.library, doc)
        doc = self.example()
        doc["devices"][0]["model"].pop("generating_bins")
        with self.assertRaises(ValueError):
            analyze_conversion_network(self.library, doc)

    def test_duplicate_model_indices(self):
        device = dict(
            channels=[(0, 1), (1, 1), (1, 3)],
            direct=[[0] * 3 for _ in range(3)],
            source=[0.01, 0, 0],
        )
        with self.assertRaises(RFModelError):
            self.library.solve_conversion_operating_point(
                1e6, [device, device], highorders=[dict(device=0, **PARAMETERS)] * 2
            )
        with self.assertRaises(RFModelError):
            self.library.solve_conversion_operating_point(
                1e6,
                [device, device],
                highorders=[dict(device=0, **PARAMETERS)],
                polynomials=[dict(device=0, voltage_coefficients=[0, 1])],
            )

    def test_four_model_types_in_one_network(self):
        doc = json.loads((ROOT / "examples/nonlinear-mixed-amplifiers.json").read_text())
        bins = [c["bin"] for c in doc["devices"][-1]["channels"] if c["port"] == 1]
        doc["devices"].append(
            dict(
                id="highorder",
                channels=[dict(port=p, bin=b) for p in (0, 1) for b in bins],
                model=dict(type="highorder_amplifier", **dict(PARAMETERS, generating_bins=[2])),
                noise=dict(noiseless=True),
            )
        )
        doc["connections"].append([["compressor", 1], ["highorder", 0]])
        doc["boundaries"] = [b for b in doc["boundaries"] if b["channel"][0] != "compressor"]
        doc["boundaries"] += [dict(channel=["highorder", 1, b]) for b in bins]
        result = analyze_conversion_network(self.library, doc)
        upstream = {
            r["bin"]: complex(*r["outgoing"])
            for r in result["channels"]
            if r["device"] == "compressor" and r["port"] == 1
        }
        expected = sampled(bins, [upstream[b] for b in bins], [0, 0.1], [2], bins)
        for row in result["channels"]:
            if row["device"] == "highorder" and row["port"] == 1:
                self.close(complex(*row["outgoing"]), expected[row["bin"]])
        self.assertLessEqual(result["operating_point"]["scaled_residual"], 1)

    def test_fixed_offset_and_additional_noise(self):
        device = dict(
            channels=[(0, 1), (1, 1), (1, 3)],
            direct=[[0] * 3 for _ in range(3)],
            source=[0.01, 0, 0],
        )
        result = self.library.solve_conversion_operating_point(
            1e6,
            [device],
            highorders=[dict(device=0, **PARAMETERS)],
            output_offset=[0, 0, 0.003j],
            additional_source_covariance=[[1e-9, 0, 0], [0, 0, 0], [0, 0, 0]],
        ).waves
        self.close(result.outgoing[2], 2.5e-6 + 0.003j)
        self.close(result.noise_covariance[2][2], 0.00075**2 * 1e-9, 1e-15)

    def test_warm_start(self):
        doc = self.example()
        result = analyze_conversion_network(self.library, doc)
        doc["operating_point"]["initial_incident"] = [r["incident"] for r in result["channels"]]
        self.assertEqual(
            analyze_conversion_network(self.library, doc)["operating_point"]["iterations"], 0
        )

    def test_noise_analysis_reconstructs_converged_model(self):
        doc = self.example()
        doc["devices"] = doc["devices"][:1]
        doc["connections"] = []
        doc["boundaries"] = [
            dict(channel=["highorder", 0, 1], source=0.03, noise_temperature_k=290),
            dict(channel=["highorder", 1, 1]),
            dict(channel=["highorder", 1, 3]),
        ]
        doc["noise_analyses"] = [
            dict(
                name="nf",
                reference_channels=[["highorder", 0, 1]],
                thermal_channels=[["highorder", 0, 1]],
                output_channel=["highorder", 1, 3],
            )
        ]
        metric = analyze_conversion_network(self.library, doc)["noise_analyses"][0]
        _, _, l, dl = scalar(0.03)
        radial, tangent = 7.5 * l * l * dl, 7.5 * l**3 / 0.03
        self.close(metric["reference_gain"], 0.5 * (radial**2 + tangent**2))
        self.close(metric["noise_factor"], 1)

    def test_cli_failure_preserves_result(self):
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
            doc["devices"][0]["model"]["generating_bins"] = [2]
            source.write_text(json.dumps(doc))
            failed = subprocess.run(command, capture_output=True, text=True, env=env)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()

"""AM-PM C/Python/JSON composition, independent feedback and failure contracts."""

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

PHASE = dict(peak_degrees_per_db=-5, output_reference_dbm=10, output_channels=[1])
AMPLIFIER = dict(power_gain_db=20, output_p1db_dbm=20, output_saturation_dbm=23)


def angle(power, k=-5, reference=10):
    if not power:
        return 0.0
    # Integrate the dB-domain derivative rather than the native closed form.
    end = min(10 * math.log10(power) + 30 - reference, 20 * math.log10(1.5))
    if end <= -160:
        return 0.0
    n = 12000
    step = (end + 160) / n
    total = 0.0
    for i in range(n + 1):
        u = 10 ** ((-160 + step * i) / 20)
        total += (1 if i in (0, n) else (4 if i % 2 else 2)) * k * u * u * (3 - 2 * u)
    return total * step / 3 * math.pi / 180


def response(a):
    r = abs(a)
    p1 = math.sqrt(10 ** (-2.9))
    q = 10 ** (-0.05)
    if r <= p1:
        m = 10 * r * (1 - (1 - q) * (r / p1) ** 2)
    else:
        head = math.sqrt(10 ** (-0.7)) - math.sqrt(0.1)
        m = math.sqrt(0.1) + head * math.tanh(10 * (3 * q - 2) * (r - p1) / head)
    return a / r * m * cmath.exp(1j * angle(m * m)) if r else 0j


class AmPmTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = Library(LIBRARY_PATH)

    def close(self, a, b, scale=1):
        self.assertLessEqual(abs(a - b), 2e-8 * max(scale, abs(b)))

    def example(self):
        return json.loads((ROOT / "examples/nonlinear-am-pm-feedback.json").read_text())

    def base(self, a):
        return self.lib.linearize_saturating_amplifier(1e6, [(0, 1), (1, 1)], [a, 0], **AMPLIFIER)

    def phased(self, a):
        return self.lib.apply_output_am_pm(1e6, [(0, 1), (1, 1)], [a, 0], self.base(a), **PHASE)

    def test_phase_response(self):
        for k in [-5, 0, 8]:
            for reference in [-20, 10, 40]:
                for offset in [-30, 0, 3, 10]:
                    p = 10 ** ((reference + offset - 30) / 10)
                    got = self.lib.am_pm_response(
                        p, peak_degrees_per_db=k, output_reference_dbm=reference
                    )
                    self.close(got.phase_rad, angle(p, k, reference))
                    if offset == 0:
                        self.close(got.radial_phase_slope * math.log(10) / 20 * 180 / math.pi, k)
        self.assertEqual(
            self.lib.am_pm_response(0, peak_degrees_per_db=-5, output_reference_dbm=10).phase_rad, 0
        )

    def test_local_response_and_derivatives(self):
        for a in [0, 0.005 + 0.002j, 0.01 + 0.003j, 0.03 + 0.01j, 0.1j]:
            point = self.phased(a)
            self.close(point.operating_outgoing[1], response(a))
            for step in [1e-7, 1e-7j]:
                derivative = (response(a + step) - response(a - step)) / (2 * abs(step))
                self.close(
                    (point.direct[1][0] * step + point.conjugate[1][0] * complex(step).conjugate())
                    / abs(step),
                    derivative,
                )
            self.close(
                point.output_offset[1]
                + point.direct[1][0] * a
                + point.conjugate[1][0] * complex(a).conjugate(),
                point.operating_outgoing[1],
            )

    def test_feedback_and_complex_noise(self):
        result = analyze_conversion_network(self.lib, self.example())
        root = 0.01 + 0.003j
        for _ in range(60):
            root = 0.01 + 0.003j - 0.03 * response(root)
        self.close(complex(*result["channels"][0]["incident"]), root)
        self.close(complex(*result["channels"][1]["outgoing"]), response(root))
        h = 1e-7
        dx = (response(root + h) - response(root - h)) / (2 * h)
        dy = (response(root + 1j * h) - response(root - 1j * h)) / (2 * h)
        m00, m01, m10, m11 = 1 + 0.03 * dx.real, 0.03 * dy.real, 0.03 * dx.imag, 1 + 0.03 * dy.imag
        det = m00 * m11 - m01 * m10
        rx = (dx * m11 - dy * m10) / det
        ry = (-dx * m01 + dy * m00) / det
        self.close(
            complex(*result["noise_covariance_w_per_hz"][1][1]),
            0.5e-9 * (abs(rx) ** 2 + abs(ry) ** 2),
            1e-9,
        )
        self.close(
            complex(*result["noise_complementary_w_per_hz"][1][1]),
            0.5e-9 * (rx * rx + ry * ry),
            1e-9,
        )

    def test_all_four_nonlinear_types(self):
        cases = [
            (
                "amplifiers",
                AMPLIFIER,
                [(0, 1), (1, 1)],
                [0.01, 0],
                self.lib.linearize_saturating_amplifier,
                [1],
            ),
            (
                "polynomials",
                dict(voltage_coefficients=[0, 2]),
                [(0, 1), (1, 1)],
                [0.01, 0],
                self.lib.linearize_polynomial_amplifier,
                [1],
            ),
            (
                "highorders",
                dict(**AMPLIFIER, nonlinear_voltage_coefficients=[0, 0.1], generating_bins=[1]),
                [(0, 1), (1, 1), (1, 3)],
                [0.01, 0, 0],
                self.lib.linearize_highorder_amplifier,
                [1, 2],
            ),
            (
                "mixers",
                dict(lo_reference_amplitude=1),
                [(0, 3), (1, 2), (2, 1), (2, 5)],
                [0.01, 1, 0, 0],
                self.lib.linearize_bilinear_mixer,
                [2, 3],
            ),
        ]
        for key, params, channels, waves, linearize, selected in cases:
            n = len(channels)
            base = linearize(1e6, channels, waves, **params)
            phase = dict(PHASE, output_channels=selected)
            local = self.lib.apply_output_am_pm(1e6, channels, waves, base, **phase)
            device = dict(channels=channels, direct=[[0] * n for _ in channels], source=waves)
            solved = self.lib.solve_conversion_operating_point(
                1e6, [device], am_pm=[dict(device=0, **phase)], **{key: [dict(device=0, **params)]}
            )
            for a, b in zip(solved.waves.outgoing, local.operating_outgoing):
                self.close(a, b)

    def test_selected_channels_only_and_identity(self):
        base = self.base(0.01)
        for fields in [dict(output_channels=[]), dict(peak_degrees_per_db=0)]:
            point = self.lib.apply_output_am_pm(
                1e6, [(0, 1), (1, 1)], [0.01, 0], base, **dict(PHASE, **fields)
            )
            self.assertEqual(point.direct, base.direct)
            self.assertEqual(point.operating_outgoing, base.operating_outgoing)

    def test_fixed_offset_after_phase(self):
        device = dict(channels=[(0, 1), (1, 1)], direct=[[0, 0], [0, 0]], source=[0.01, 0])
        result = self.lib.solve_conversion_operating_point(
            1e6,
            [device],
            amplifiers=[dict(device=0, **AMPLIFIER)],
            am_pm=[dict(device=0, **PHASE)],
            output_offset=[0, 0.003j],
        ).waves
        self.close(result.outgoing[1], response(0.01) + 0.003j)

    def test_nf_rebuild_uses_phase_derivative(self):
        doc = self.example()
        doc["devices"] = doc["devices"][:1]
        doc["connections"] = []
        doc["boundaries"] = [
            dict(channel=["amplifier", 0, 1], source=0.01, noise_temperature_k=290),
            dict(channel=["amplifier", 1, 1]),
        ]
        doc["noise_analyses"] = [
            dict(
                name="nf",
                reference_channels=[["amplifier", 0, 1]],
                thermal_channels=[["amplifier", 0, 1]],
                output_channel=["amplifier", 1, 1],
            )
        ]
        metric = analyze_conversion_network(self.lib, doc)["noise_analyses"][0]
        h = 1e-7
        dx = (response(0.01 + h) - response(0.01 - h)) / (2 * h)
        dy = (response(0.01 + 1j * h) - response(0.01 - 1j * h)) / (2 * h)
        self.close(metric["reference_gain"], 0.5 * (abs(dx) ** 2 + abs(dy) ** 2))
        self.close(metric["noise_factor"], 1)

    def test_warm_start(self):
        doc = self.example()
        result = analyze_conversion_network(self.lib, doc)
        doc["operating_point"]["initial_incident"] = [r["incident"] for r in result["channels"]]
        self.assertEqual(
            analyze_conversion_network(self.lib, doc)["operating_point"]["iterations"], 0
        )

    def test_invalid_local_parameters(self):
        for fields in [
            dict(output_channels=[1, 1]),
            dict(output_channels=[2]),
            dict(output_channels=[True]),
            dict(output_channels="1"),
            dict(peak_degrees_per_db=True),
            dict(output_reference_dbm=math.nan),
            dict(peak_degrees_per_db=math.inf),
        ]:
            with (
                self.subTest(fields=fields),
                self.assertRaises((ValueError, TypeError, RFModelError)),
            ):
                self.lib.apply_output_am_pm(
                    1, [(0, 1), (1, 1)], [0.01, 0], self.base(0.01), **dict(PHASE, **fields)
                )
        with self.assertRaises(RFModelError):
            self.lib.am_pm_response(-1, peak_degrees_per_db=-5, output_reference_dbm=10)
        with self.assertRaises(TypeError):
            self.lib.am_pm_response(True, peak_degrees_per_db=-5, output_reference_dbm=10)

    def test_reject_dc_and_dimensions(self):
        base = self.lib.linearize_polynomial_amplifier(
            1, [(0, 0), (1, 0)], [0.1, 0], voltage_coefficients=[0, 2]
        )
        with self.assertRaises(RFModelError):
            self.lib.apply_output_am_pm(1, [(0, 0), (1, 0)], [0.1, 0], base, **PHASE)
        with self.assertRaises(ValueError):
            self.lib.apply_output_am_pm(1, [(0, 1), (1, 1)], [0.1], self.base(0.1), **PHASE)

    def test_invalid_network_mapping(self):
        device = dict(channels=[(0, 1), (1, 1)], direct=[[0, 0], [0, 0]], source=[0.01, 0])
        for phases in [
            [dict(device=1, **PHASE)],
            [dict(device=0, **PHASE)] * 2,
            [dict(device=True, **PHASE)],
        ]:
            with self.assertRaises((ValueError, TypeError, RFModelError)):
                self.lib.solve_conversion_operating_point(
                    1, [device, device], amplifiers=[dict(device=0, **AMPLIFIER)], am_pm=phases
                )

    def test_multiple_decorated_devices_and_duplicate_rejection(self):
        device = dict(channels=[(0, 1), (1, 1)], direct=[[0, 0], [0, 0]], source=[0.01, 0])
        models = [dict(device=i, **AMPLIFIER) for i in range(2)]
        phases = [
            dict(device=i, **dict(PHASE, peak_degrees_per_db=(-5 if i == 0 else 5)))
            for i in range(2)
        ]
        result = self.lib.solve_conversion_operating_point(
            1e6, [device, device], amplifiers=models, am_pm=phases
        ).waves
        self.close(result.outgoing[1], result.outgoing[3].conjugate())
        with self.assertRaises(RFModelError):
            self.lib.solve_conversion_operating_point(
                1e6, [device, device], amplifiers=models, am_pm=[phases[0]] * 2
            )
        with self.assertRaises(ValueError):
            self.lib.apply_output_am_pm(
                1, [(0, 1), (1, 1)], [math.nan, 0], self.base(0.01), **PHASE
            )

    def test_json_strictness(self):
        for fields in [
            dict(extra=1),
            dict(output_channels="1"),
            dict(output_channels=[True]),
            dict(peak_degrees_per_db=False),
        ]:
            doc = self.example()
            doc["devices"][0]["am_pm"].update(fields)
            with self.assertRaises((ValueError, TypeError, RFModelError)):
                analyze_conversion_network(self.lib, doc)
        doc = self.example()
        doc.pop("operating_point")
        with self.assertRaises(ValueError):
            analyze_conversion_network(self.lib, doc)
        doc = self.example()
        doc["devices"][0]["model"] = dict(type="matrix", direct=[[0, 0], [0, 0]])
        with self.assertRaises(ValueError):
            analyze_conversion_network(self.lib, doc)

    def test_cli_failure_preserves_result(self):
        with tempfile.TemporaryDirectory() as directory:
            source, output = Path(directory) / "input.json", Path(directory) / "output.json"
            doc = self.example()
            source.write_text(json.dumps(doc))
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
            env = dict(os.environ, PYTHONPATH=str(Path(rfmodel.__file__).parent.parent))
            first = subprocess.run(command, capture_output=True, text=True, env=env)
            self.assertEqual(first.returncode, 0, first.stderr)
            saved = output.read_bytes()
            doc["devices"][0]["am_pm"]["output_channels"] = [2]
            source.write_text(json.dumps(doc))
            failed = subprocess.run(command, capture_output=True, text=True, env=env)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(saved, output.read_bytes())


if __name__ == "__main__":
    unittest.main()

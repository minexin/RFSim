"""Independent Fourier-product Jacobian and RF/LO phase-noise regression."""

import copy
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
from rfmodel.mixer_linearization_file import analyze_mixer_linearization


def exact_product(channels, waves, normalization):
    # Independent signed Fourier convolution, RMS-to-Fourier at both boundaries.
    spectra = [{}, {}]
    for (port, index), wave in zip(channels, waves):
        if port >= 2:
            continue
        if index == 0:
            spectra[port][0] = complex(wave).real
        else:
            spectra[port][index] = wave / math.sqrt(2)
            spectra[port][-index] = complex(wave).conjugate() / math.sqrt(2)
    product = {}
    for r, x in spectra[0].items():
        for l, y in spectra[1].items():
            product[r + l] = product.get(r + l, 0j) + math.sqrt(2) * normalization * x * y
    return [
        product.get(index, 0j) * (1 if index == 0 else math.sqrt(2)) if port == 2 else 0j
        for port, index in channels
    ]


def apply(linearization, delta):
    return [
        sum(a * x + b * complex(x).conjugate() for a, b, x in zip(ar, br, delta))
        for ar, br in zip(linearization.direct, linearization.conjugate)
    ]


class MixerLinearizationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def close(self, value, expected, scale=1, tolerance=4e-12):
        self.assertLessEqual(abs(value - expected), tolerance * max(scale, abs(expected)))

    def point(self):
        channels = (
            [(0, b) for b in [0, 2, 5]]
            + [(1, b) for b in [0, 3, 4]]
            + [(2, b) for b in [0, 1, 2, 3, 4, 5, 6, 8, 9]]
        )
        operating = [0.2, 0.7 + 0.1j, -0.3 + 0.2j, 0, 0.8 - 0.4j, 0] + [0] * 9
        return channels, operating

    def test_nominal_and_all_quadrature_derivatives_against_fourier_convolution(self):
        channels, operating = self.point()
        model = self.library.linearize_real_mixer(1, channels, operating, lo_bin=3, gain_db=-3)
        k = 10 ** (-3 / 20) / abs(operating[4])
        expected = exact_product(channels, operating, k)
        for actual, value in zip(model.operating_outgoing, expected):
            self.close(actual, value)
        for column, (port, index) in enumerate(channels):
            for direction in ([1] if index == 0 else [1, 1j]):
                eps = 1e-6
                plus, minus = operating[:], operating[:]
                plus[column] += eps * direction
                minus[column] -= eps * direction
                derivative = [
                    (a - b) / (2 * eps)
                    for a, b in zip(
                        exact_product(channels, plus, k), exact_product(channels, minus, k)
                    )
                ]
                delta = [0j] * len(channels)
                delta[column] = direction
                for actual, value in zip(apply(model, delta), derivative):
                    self.close(actual, value, tolerance=2e-9)
        # A bilinear map obeys J(x0)*x0 = 2*f(x0); J is not an absolute-wave model.
        for actual, value in zip(apply(model, operating), expected):
            self.close(actual, 2 * value)

    def test_second_order_residual_scales_quadratically(self):
        channels, operating = self.point()
        model = self.library.linearize_real_mixer(1, channels, operating, lo_bin=3)
        errors = []
        for eps in [1e-2, 5e-3, 2.5e-3]:
            delta = [0j] * len(channels)
            delta[1], delta[5] = eps * (0.4 + 0.2j), eps * (0.3 - 0.1j)
            actual = exact_product(
                channels, [a + b for a, b in zip(operating, delta)], 1 / abs(operating[4])
            )
            incremental = apply(model, delta)
            errors.append(
                max(
                    abs(x - y - z) for x, y, z in zip(actual, model.operating_outgoing, incremental)
                )
            )
        self.close(errors[0] / errors[1], 4, tolerance=1e-9)
        self.close(errors[1] / errors[2], 4, tolerance=1e-9)

    def basic(self, rf_bin=12, lo_amplitude=1, rf_amplitude=1):
        lo_bin = 10
        rf_bins = [rf_bin - 1, rf_bin, rf_bin + 1]
        lo_bins = [9, 10, 11]
        outputs = sorted(
            {r + 10 for r in rf_bins}
            | {abs(r - 10) for r in rf_bins}
            | {rf_bin + l for l in lo_bins}
            | {abs(rf_bin - l) for l in lo_bins}
        )
        channels = [(0, b) for b in rf_bins] + [(1, b) for b in lo_bins] + [(2, b) for b in outputs]
        waves = [
            rf_amplitude if p == 0 and b == rf_bin else lo_amplitude if p == 1 and b == 10 else 0
            for p, b in channels
        ]
        return channels, waves

    def phase_result(
        self, gains=(1, 1), rf_bin=12, lo_amplitude=1, rf_amplitude=1, independent=False
    ):
        channels, waves = self.basic(rf_bin, lo_amplitude, rf_amplitude)
        model = self.library.linearize_real_mixer(1e6, channels, waves, lo_bin=10)
        members = [(1, rf_amplitude, gains[0]), (4, lo_amplitude, gains[1])]
        n = len(channels)
        c, p = [[0j] * n for _ in range(n)], [[0j] * n for _ in range(n)]
        for group in ([members] if not independent else [[m] for m in members]):
            gc, gp = self.library.phase_noise_group(channels, group, offsets=[(1, -100)])
            for i in range(n):
                for j in range(n):
                    c[i][j] += gc[i][j]
                    p[i][j] += gp[i][j]
        result = self.library.frequency_conversion(
            1e6,
            channels,
            model.direct,
            model.conjugate,
            source_covariance=c,
            source_complementary=p,
        )
        return channels, model, result

    def test_shared_phase_sum_and_difference_gains(self):
        for rf_bin in [8, 12]:
            for gains in [(1, 1), (1, -1), (12, 10), (1, 0), (0, 1)]:
                channels, model, result = self.phase_result(gains, rf_bin)
                difference = abs(rf_bin - 10)
                for bin_index, gain in [
                    (difference - 1, gains[0] - gains[1]),
                    (difference + 1, gains[0] - gains[1]),
                    (rf_bin + 9, gains[0] + gains[1]),
                    (rf_bin + 11, gains[0] + gains[1]),
                ]:
                    i = channels.index((2, bin_index))
                    self.close(result.noise_covariance[i][i], gain * gain * 1e-10, 1e-10)

    def test_incremental_network_with_upstream_filter_and_shared_lo(self):
        channels, operating = self.basic(rf_amplitude=0.5)
        mixer = self.library.linearize_real_mixer(1e6, channels, operating, lo_bin=10)
        filter_channels = [(port, b) for b in [11, 12, 13] for port in [0, 1]]
        direct = [[0j] * 6 for _ in range(6)]
        for i in range(0, 6, 2):
            direct[i][i + 1] = direct[i + 1][i] = 0.5
        devices = [
            dict(channels=filter_channels, direct=direct),
            dict(channels=channels, direct=mixer.direct, conjugate=mixer.conjugate),
        ]
        global_channels = filter_channels + [(p + 2, b) for p, b in channels]
        c, p = self.library.phase_noise_group(
            global_channels, [(2, 1, 1), (10, 1, 1)], offsets=[(1, -100)]
        )
        result = self.library.conversion_network(
            1e6,
            devices,
            [((0, 1), (1, 0))],
            additional_source_covariance=c,
            additional_source_complementary=p,
            loaded_noise=True,
        )
        for bin_index, expected in [(1, 0), (3, 0), (21, 1e-10), (23, 1e-10)]:
            i = 6 + channels.index((2, bin_index))
            self.close(result.noise_covariance[i][i], expected, 1e-10)
        self.close(mixer.operating_outgoing[channels.index((2, 2))], 0.5)

    def test_independent_rf_lo_phases_add(self):
        channels, model, result = self.phase_result(independent=True)
        for bin_index in [1, 3, 21, 23]:
            i = channels.index((2, bin_index))
            self.close(result.noise_covariance[i][i], 2e-10, 1e-10)

    def test_complex_carriers_and_lo_amplitude_normalization(self):
        rf = 1.7 * cmath.exp(0.31j)
        for lo in [0.2 * cmath.exp(-0.41j), 5 * cmath.exp(-0.41j)]:
            channels, model, result = self.phase_result((3, 1), lo_amplitude=lo, rf_amplitude=rf)
            lower, upper = channels.index((2, 1)), channels.index((2, 3))
            nominal = rf * complex(lo).conjugate() / abs(lo)
            self.close(model.operating_outgoing[channels.index((2, 2))], nominal)
            self.close(result.noise_covariance[upper][upper], abs(rf) ** 2 * 4e-10, 1e-10)
            self.close(result.noise_complementary[lower][upper], -(nominal**2) * 4e-10, 1e-10)

    def test_reciprocal_mixing_of_strong_blocker(self):
        channels = (
            [(0, 13)] + [(1, b) for b in [8, 10, 12]] + [(2, b) for b in [1, 3, 5, 21, 23, 25]]
        )
        waves = [10, 0, 2, 0] + [0] * 6
        model = self.library.linearize_real_mixer(1e6, channels, waves, lo_bin=10)
        c, p = self.library.phase_noise_group(channels, [(2, 2, 1)], offsets=[(2, -100)])
        result = self.library.frequency_conversion(
            1e6,
            channels,
            model.direct,
            model.conjugate,
            source_covariance=c,
            source_complementary=p,
        )
        i = channels.index((2, 1))
        self.assertEqual(model.operating_outgoing[i], 0)
        self.close(result.noise_covariance[i][i], 1e-8, 1e-8)

    def test_rf_derivative_reduces_to_existing_fixed_pump(self):
        channels, waves = self.basic(lo_amplitude=2j)
        model = self.library.linearize_real_mixer(1, channels, waves, lo_bin=10, gain_db=-4)
        old_channels = [(0, b) if p == 0 else (1, b) for p, b in channels if p != 1]
        a, b = self.library.ideal_mixer_conversion(
            1, old_channels, lo_bin=10, gain_db=-4, phase_radians=math.pi / 2
        )
        indices = [i for i, (p, _) in enumerate(channels) if p != 1]
        for i, old_i in enumerate(indices):
            for j, old_j in enumerate(indices):
                self.close(model.direct[old_i][old_j], a[i][j])
                self.close(model.conjugate[old_i][old_j], b[i][j])

    def test_same_frequency_dc_and_phase_detector(self):
        channels, waves = self.basic(rf_bin=10, rf_amplitude=1j)
        model = self.library.linearize_real_mixer(1, channels, waves, lo_bin=10)
        self.close(model.operating_outgoing[channels.index((2, 0))], 0)
        for gains, expected in [((1, 1), 0), ((1, 0), 4e-10), ((0, 1), 4e-10)]:
            _, _, result = self.phase_result(gains, rf_bin=10, rf_amplitude=1j)
            self.close(
                result.noise_covariance[channels.index((2, 1))][channels.index((2, 1))],
                expected,
                1e-10,
            )

    def test_invalid_operating_points_and_incremental_closure(self):
        channels, waves = self.basic()
        for change in [
            lambda w: w.__setitem__(4, 0),
            lambda w: w.__setitem__(3, 1),
            lambda w: w.__setitem__(1, math.nan),
        ]:
            bad = waves[:]
            change(bad)
            with self.assertRaises(RFModelError):
                self.library.linearize_real_mixer(1, channels, bad, lo_bin=10)
        for options in [dict(rf_port=1), dict(gain_db=math.inf), dict(reference_ohms=-1)]:
            with self.assertRaises(RFModelError):
                self.library.linearize_real_mixer(1, channels, waves, lo_bin=10, **options)
        channels = [(0, 12), (1, 9), (1, 10), (1, 11)] + [(2, b) for b in [2, 3, 21, 22, 23]]
        with self.assertRaises(RFModelError):
            self.library.linearize_real_mixer(1, channels, [1, 0, 1, 0] + [0] * 5, lo_bin=10)

    def example(self):
        return json.loads((ROOT / "examples/mixer-lo-phase-noise.json").read_text())

    def test_json_operating_and_incremental_results_are_separate(self):
        result = analyze_mixer_linearization(self.library, self.example())
        self.assertEqual(result["analysis"], "incremental_about_supplied_operating_point")
        for row in result["channels"]:
            self.assertEqual(row["perturbation_outgoing"], [0, 0])
            if row["port"] == 2 and row["bin"] in [2, 22]:
                self.assertEqual(row["operating_outgoing"], [1, 0])
        index = next(
            i for i, r in enumerate(result["channels"]) if r["port"] == 2 and r["bin"] == 23
        )
        self.close(complex(*result["noise_covariance_w_per_hz"][index][index]), 4e-10, 1e-10)
        self.assertIn("incident_outgoing_noise_covariance_w_per_hz", result)

    def test_json_rejects_ambiguous_or_unknown_inputs(self):
        for mutation in [
            lambda d: d.update(source=[0] * 12),
            lambda d: d.update(loaded_noise=1),
            lambda d: d["model"].update(lo_bin=True),
            lambda d: d.update(operating_incident=[]),
            lambda d: d["channels"][0].update(port=True),
        ]:
            document = self.example()
            mutation(document)
            with self.assertRaises((ValueError, RFModelError)):
                analyze_mixer_linearization(self.library, document)

    def test_cli_failure_preserves_result(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / "input.json", Path(folder) / "output.json"
            source.write_text(json.dumps(self.example()))
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
            document = self.example()
            document["operating_incident"][4] = 0
            source.write_text(json.dumps(document))
            self.assertNotEqual(subprocess.run(command, capture_output=True, env=env).returncode, 0)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()

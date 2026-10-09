"""Phase noise statistics, quadrature detection, independent sources and API guards."""

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

LIBRARY_PATH = str(Path(sys.argv.pop(1)).resolve())
ROOT = Path(__file__).resolve().parents[1]
if "--installed" in sys.argv:
    sys.argv.remove("--installed")
else:
    sys.path.insert(0, str(ROOT / "python"))
import rfmodel
from rfmodel import Library, RFModelError
from rfmodel.conversion_network_file import analyze_conversion_network


def diagonal(values):
    return [[value if i == j else 0j for j in range(len(values))] for i, value in enumerate(values)]


class PhaseNoiseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def noise(self, wave=1, channels=None, offsets=None, carrier_channel=1):
        return self.library.phase_noise_sidebands(
            channels or [(0, 9), (0, 10), (0, 11)],
            carrier_channel=carrier_channel,
            carrier_wave=wave,
            offsets=offsets or [(1, -100)],
        )

    def close(self, actual, expected, scale=1e-10):
        self.assertLessEqual(abs(actual - expected), max(abs(expected), scale) * 2e-12)

    def test_statistics_from_independent_four_phase_ensemble(self):
        wave = 1 + 2j
        c, p = self.noise(wave)
        # A unit proper random phasor has E[u^2]=0, E[|u|^2]=1.
        ensemble = [
            [1j * wave * u.conjugate(), 0j, 1j * wave * u]
            for u in [1e-5 + 0j, 1e-5j, -1e-5 + 0j, -1e-5j]
        ]
        for i in range(3):
            for j in range(3):
                expected_c = sum(x[i] * x[j].conjugate() for x in ensemble) / 4
                expected_p = sum(x[i] * x[j] for x in ensemble) / 4
                self.close(c[i][j], expected_c)
                self.close(p[i][j], expected_p)

    def test_carrier_phase_rotates_complementary_twice(self):
        c, p = self.noise(2)
        d, q = self.noise(2 * cmath.exp(0.731j))
        for i in range(3):
            for j in range(3):
                self.close(d[i][j], c[i][j])
                self.close(q[i][j], p[i][j] * cmath.exp(1.462j))

    def test_unordered_channels_and_independent_offsets(self):
        channels = [(4, 12), (0, 10), (4, 10), (4, 8), (4, 9), (4, 11)]
        c, p = self.noise(channels=channels, carrier_channel=2, offsets=[(1, -100), (2, -110)])
        self.close(c[0][0], 1e-11)
        self.close(p[0][3], -1e-11)
        self.close(p[4][5], -1e-10)
        self.assertEqual(c[1][1], 0)
        self.assertEqual(p[0][5], 0)

    def test_linear_filter_and_complex_reflection_closed_form(self):
        channels = [(0, 9), (0, 10), (0, 11)]
        c, p = self.noise(1 + 2j)
        transfer = [0.3 + 0.2j, 0.2, -0.1 + 0.6j]
        gamma = [0.2j, 0, -0.1j]
        result = self.library.frequency_conversion(
            1e6,
            channels,
            diagonal(transfer),
            diagonal([0] * 3),
            source=[0, 1 + 2j, 0],
            reflection=gamma,
            source_covariance=c,
            source_complementary=p,
        )
        h = [t / (1 - t * g) for t, g in zip(transfer, gamma)]
        for i in range(3):
            for j in range(3):
                self.close(result.noise_covariance[i][j], h[i] * c[i][j] * h[j].conjugate())
                self.close(result.noise_complementary[i][j], h[i] * p[i][j] * h[j])
        self.close(result.outgoing[1], transfer[1] * (1 + 2j), scale=1)

    def detector(self, phase, discard_pair=False):
        channels = [(0, b) for b in [9, 10, 11]] + [(1, b) for b in [0, 1, 19, 20, 21]]
        c, p = self.noise(channels=channels)
        a, b = self.library.ideal_mixer_conversion(1e6, channels, lo_bin=10, phase_radians=phase)
        return self.library.frequency_conversion(
            1e6,
            channels,
            a,
            b,
            source=[0, 1, 0, 0, 0, 0, 0, 0],
            source_covariance=c,
            source_complementary=None if discard_pair else p,
        )

    def test_in_phase_mixer_rejects_first_order_pm(self):
        result = self.detector(0)
        self.close(result.noise_covariance[4][4], 0)
        self.close(result.outgoing[3], math.sqrt(2), scale=1)
        # Demonstrates why diagonal independent noise is physically insufficient.
        self.close(self.detector(0, True).noise_covariance[4][4], 2e-10)

    def test_quadrature_mixer_detects_pm_and_phase_scan(self):
        for phase in [0, 0.3, 0.8, math.pi / 2, math.pi]:
            result = self.detector(phase)
            self.close(result.noise_covariance[4][4], 4e-10 * math.sin(phase) ** 2)
            self.close(result.noise_complementary[4][4], 0)

    def test_log_scaling_avoids_intermediate_overflow(self):
        self.close(self.noise(1e200, offsets=[(1, -4000)])[0][0][0], 1, scale=1)
        self.close(self.noise(1e-200, offsets=[(1, 4000)])[0][0][0], 1, scale=1)
        for level in [-1e308, 1e308]:
            with self.assertRaises(RFModelError):
                self.noise(offsets=[(1, level)])

    def test_tiny_carrier_has_well_normalized_phase(self):
        carrier = complex(1e-320, 1e-320)
        c, p = self.noise(carrier, offsets=[(1, 6400)])
        expected = math.exp(2 * math.log(carrier.real) + math.log(2) + 640 * math.log(10))
        self.close(c[0][0], expected, scale=1)
        self.close(p[0][2], -1j * expected, scale=1)
        with self.assertRaises(RFModelError):
            self.noise(offsets=[(1, -3200)])

    def test_invalid_carrier_channels_and_offsets(self):
        variants = [
            dict(wave=0),
            dict(wave=float("nan")),
            dict(wave=complex(0, math.inf)),
            dict(carrier_channel=3),
            dict(channels=[(0, 9), (0, 0), (0, 11)]),
            dict(channels=[(0, 9), (0, 10), (1, 11)]),
            dict(channels=[(0, 10), (0, 10), (0, 11)]),
            dict(offsets=[(0, -100)]),
            dict(offsets=[(10, -100)]),
            dict(offsets=[(1, -100), (1, -110)]),
            dict(offsets=[(1, math.inf)]),
            dict(channels=[(0, 2147483646), (0, 2147483647)], offsets=[(1, -100)]),
        ]
        for arguments in variants:
            with self.subTest(arguments=arguments), self.assertRaises((ValueError, RFModelError)):
                self.noise(**arguments)
        with self.assertRaises(ValueError):
            self.library.phase_noise_sidebands(
                [(0, 10)], carrier_channel=0, carrier_wave=1, offsets=[]
            )

    def example(self):
        return json.loads((ROOT / "examples/phase-noise-detector.json").read_text())

    def test_json_detector_and_offset_band_integration(self):
        result = analyze_conversion_network(self.library, self.example())
        indices = {(c["port"], c["bin"]): i for i, c in enumerate(result["channels"])}
        for offset, level in [(1, 1e-10), (2, 1e-11)]:
            index = indices[1, offset]
            self.close(complex(*result["noise_covariance_w_per_hz"][index][index]), 4 * level)
        metric = result["channel_measurements"][0]
        self.close(metric["noise_power_w"], 0.5 * (4e-10 + 4e-11) * 1e6, scale=1e-4)
        self.assertEqual(result["phase_noise_sources"][0]["model"], "small_angle_paired_sidebands")
        original = self.example()
        del original["phase_noise_sources"]
        baseline = analyze_conversion_network(self.library, original)
        self.assertEqual(result["channels"], baseline["channels"])

    def test_independent_sources_add_even_with_overlapping_sidebands(self):
        bins = [9, 10, 11, 12, 13]
        document = dict(
            format="rfmodel.conversion-network",
            version=1,
            spacing_hz=1,
            devices=[
                dict(
                    id="port",
                    bins=bins,
                    model=dict(type="linear", s=[[1]]),
                    noise=dict(noiseless=True),
                )
            ],
            boundaries=[
                dict(channel=["port", 0, b], source={10: 1, 12: 2}.get(b, 0), noise_w_per_hz=2e-11)
                for b in bins
            ],
            phase_noise_sources=[
                dict(
                    name=str(b),
                    carrier_channel=["port", 0, b],
                    offsets=[dict(offset_bin=1, ssb_dbc_per_hz=-100)],
                )
                for b in [10, 12]
            ],
        )
        result = analyze_conversion_network(self.library, document)
        c, p = result["noise_covariance_w_per_hz"], result["noise_complementary_w_per_hz"]
        self.close(complex(*c[2][2]), 5.2e-10)
        self.close(complex(*p[0][2]), -1e-10)
        self.close(complex(*p[2][4]), -4e-10)
        self.close(complex(*p[0][4]), 0)

    def test_json_strict_validation(self):
        mutations = [
            lambda d: d["phase_noise_sources"].append(copy.deepcopy(d["phase_noise_sources"][0])),
            lambda d: d["phase_noise_sources"][0].update(unknown=True),
            lambda d: d["phase_noise_sources"][0].update(carrier_channel=["unknown", 0, 10]),
            lambda d: d["phase_noise_sources"][0].update(carrier_channel=["detector", True, 10]),
            lambda d: d["phase_noise_sources"][0]["offsets"][0].update(offset_bin=True),
            lambda d: d["phase_noise_sources"][0]["offsets"][0].update(ssb_dbc_per_hz=True),
            lambda d: d["phase_noise_sources"][0].update(offsets=[]),
            lambda d: d["phase_noise_sources"][0].update(carrier_channel=["detector", 0, 8]),
        ]
        for mutation in mutations:
            document = self.example()
            mutation(document)
            with self.subTest(document=document), self.assertRaises((ValueError, RFModelError)):
                analyze_conversion_network(self.library, document)

    def test_cli_failure_preserves_existing_result(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / "source.json", Path(folder) / "result.json"
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
            document["phase_noise_sources"][0]["offsets"][0]["offset_bin"] = 10
            source.write_text(json.dumps(document))
            self.assertNotEqual(subprocess.run(command, capture_output=True, env=env).returncode, 0)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()

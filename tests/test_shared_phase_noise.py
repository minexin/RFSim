"""Cross-device noise, shared clock cancellation, and independent validation."""

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


def zero(n):
    return [[0j] * n for _ in range(n)]


class SharedPhaseNoiseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def close(self, value, expected, scale=1):
        self.assertLessEqual(abs(value - expected), 3e-12 * max(scale, abs(expected)))

    def test_group_matches_independent_latent_ensemble(self):
        channels = [(p, b) for p in [0, 1] for b in [9, 10, 11]]
        carriers = [(1, 1 + 2j, 2), (4, -0.3 + 0.7j, -0.5)]
        c, p = self.library.phase_noise_group(channels, carriers, offsets=[(1, -100)])
        samples = []
        for u in [1e-5 + 0j, 1e-5j, -1e-5 + 0j, -1e-5j]:
            samples.append(
                [
                    1j * (1 + 2j) * 2 * u.conjugate(),
                    0j,
                    1j * (1 + 2j) * 2 * u,
                    1j * (-0.3 + 0.7j) * -0.5 * u.conjugate(),
                    0j,
                    1j * (-0.3 + 0.7j) * -0.5 * u,
                ]
            )
        for i in range(6):
            for j in range(6):
                self.close(c[i][j], sum(x[i] * x[j].conjugate() for x in samples) / 4, 1e-10)
                self.close(p[i][j], sum(x[i] * x[j] for x in samples) / 4, 1e-10)

    def test_coincident_upper_lower_sidebands_are_improper(self):
        channels = [(0, b) for b in [9, 10, 11, 12, 13]]
        c, p = self.library.phase_noise_group(channels, [(1, 1, 1), (3, 2, 1)], offsets=[(1, -100)])
        self.close(c[2][2], 5e-10, 1e-10)
        self.close(p[2][2], -4e-10, 1e-10)
        self.close(c[0][2], 2e-10, 1e-10)
        self.close(p[0][4], -2e-10, 1e-10)

    def test_signed_zero_and_frequency_scaling_gains(self):
        channels = [(p, b) for p in [0, 1] for b in [9, 10, 11]]
        for gain in [0, -2, 3]:
            c, p = self.library.phase_noise_group(
                channels, [(1, 1, 1), (4, 1, gain)], offsets=[(1, -100)]
            )
            self.close(c[5][5], gain * gain * 1e-10, 1e-10)
            self.close(c[2][5], gain * 1e-10, 1e-10)
            self.close(p[2][3], -gain * 1e-10, 1e-10)

    def test_invalid_group_members_and_offsets(self):
        channels = [(0, b) for b in [9, 10, 11]]
        for carriers in [[], [(1, 1, math.nan)], [(1, 0, 1)], [(1, 1, 1), (1, 1, 2)], [(3, 1, 1)]]:
            with self.subTest(carriers=carriers), self.assertRaises((ValueError, RFModelError)):
                self.library.phase_noise_group(channels, carriers, offsets=[(1, -100)])
        with self.assertRaises(RFModelError):
            self.library.phase_noise_group(channels, [(1, 1, 0)], offsets=[(2, -100)])

    def devices(self, bins=(1, 1)):
        return [
            dict(channels=[(0, b)], direct=[[d]], reflection=[g], source_covariance=[[0.2]])
            for b, d, g in zip(bins, [0.4 + 0.1j, 0.3 - 0.2j], [0.1j, -0.2j])
        ]

    def test_cross_device_complex_reflection_and_loaded_closed_form(self):
        devices = self.devices()
        u, v = [1, 2j], [0.3j, 0.1]
        c = [
            [u[i] * u[j].conjugate() + v[i] * v[j].conjugate() for j in range(2)] for i in range(2)
        ]
        p = [[u[i] * v[j] + v[i] * u[j] for j in range(2)] for i in range(2)]
        result = self.library.conversion_network(
            1,
            devices,
            loaded_noise=True,
            additional_source_covariance=c,
            additional_source_complementary=p,
        )
        ordinary = self.library.conversion_network(
            1, devices, additional_source_covariance=c, additional_source_complementary=p
        )
        self.assertEqual(ordinary.noise_covariance, result.noise_covariance)
        h = [d["direct"][0][0] / (1 - d["direct"][0][0] * d["reflection"][0]) for d in devices]
        k = [1 / (1 - d["direct"][0][0] * d["reflection"][0]) for d in devices]
        for i in range(2):
            for j in range(2):
                total_c = c[i][j] + (0.2 if i == j else 0)
                self.close(result.noise_covariance[i][j], h[i] * total_c * h[j].conjugate())
                self.close(result.noise_complementary[i][j], h[i] * p[i][j] * h[j])
                self.close(
                    result.incident_noise_covariance[i][j], k[i] * total_c * k[j].conjugate()
                )
                self.close(
                    result.incident_outgoing_noise_covariance[i][j],
                    k[i] * total_c * h[j].conjugate(),
                )
                self.close(
                    result.incident_outgoing_noise_complementary[i][j], k[i] * p[i][j] * h[j]
                )
            self.close(
                result.net_noise_into_device_w_per_hz[i],
                (abs(k[i]) ** 2 - abs(h[i]) ** 2) * (c[i][i] + 0.2),
            )

    def test_invalid_noise_cannot_be_hidden_by_independent_positive_noise(self):
        devices = self.devices()
        with self.assertRaises(RFModelError):
            self.library.conversion_network(
                1, devices, additional_source_covariance=[[-0.1, 0], [0, 0.1]]
            )
        devices[0]["source_covariance"] = [[-0.1]]
        with self.assertRaises(RFModelError):
            self.library.conversion_network(
                1, devices, additional_source_covariance=[[10, 0], [0, 10]]
            )
        for options in [
            dict(additional_source_covariance=[[1]]),
            dict(additional_source_complementary=[[0, 0], [0, 0]]),
            dict(additional_source_covariance=[[1, 2], [2, 1]]),
        ]:
            with self.assertRaises((ValueError, RFModelError)):
                self.library.conversion_network(1, self.devices(), **options)

    def test_noise_on_connected_channels_is_rejected(self):
        devices = [dict(channels=[(0, 1)], direct=[[0.2]]) for _ in range(2)]
        with self.assertRaises(RFModelError):
            self.library.conversion_network(
                1, devices, [((0, 0), (1, 0))], additional_source_covariance=[[1, 0], [0, 1]]
            )

    def test_dc_requires_real_source_noise(self):
        devices = [dict(channels=[(0, 0)], direct=[[0.5]])]
        with self.assertRaises(RFModelError):
            self.library.conversion_network(1, devices, additional_source_covariance=[[1]])
        result = self.library.conversion_network(
            1, devices, additional_source_covariance=[[1]], additional_source_complementary=[[1]]
        )
        self.close(result.noise_covariance[0][0], 0.25)
        self.close(result.noise_complementary[0][0], 0.25)

    def example(self):
        return json.loads((ROOT / "examples/shared-phase-noise.json").read_text())

    def output_noise(self, document):
        result = analyze_conversion_network(self.library, document)
        i = next(
            i
            for i, row in enumerate(result["channels"])
            if (row["device"], row["port"], row["bin"]) == ("difference", 2, 11)
        )
        return complex(*result["noise_covariance_w_per_hz"][i][i]), result

    def test_shared_clock_cancellation_and_gain_difference(self):
        for gain in [1, 2, -1, 0]:
            document = self.example()
            document["phase_noise_groups"][0]["carriers"][1]["phase_gain"] = gain
            noise, result = self.output_noise(document)
            self.close(noise, 0.5 * (1 - gain) ** 2 * 1e-10, 1e-10)
            self.assertEqual(
                result["phase_noise_groups"][0]["correlation"], "shared_reference_phase"
            )

    def test_independent_groups_and_residual_noise_add(self):
        document = self.example()
        group = document["phase_noise_groups"].pop()
        document["phase_noise_groups"] = [
            dict(group, name=str(i), carriers=[member])
            for i, member in enumerate(group["carriers"])
        ]
        self.close(self.output_noise(document)[0], 1e-10, 1e-10)
        document = self.example()
        document["phase_noise_sources"] = [
            dict(
                name="residual",
                carrier_channel=["left", 0, 10],
                offsets=[dict(offset_bin=1, ssb_dbc_per_hz=-110)],
            )
        ]
        self.close(self.output_noise(document)[0], 0.5e-11, 1e-10)

    def test_json_strict_boundaries(self):
        mutations = [
            lambda d: d.update(additional_source_noise={"noiseless": True}),
            lambda d: d["phase_noise_groups"][0]["carriers"][0].update(phase_gain=True),
            lambda d: d["phase_noise_groups"][0]["carriers"][0].update(
                carrier_channel=["left", 1, 10]
            ),
            lambda d: d["phase_noise_groups"][0]["carriers"].append(
                copy.deepcopy(d["phase_noise_groups"][0]["carriers"][0])
            ),
            lambda d: d["phase_noise_groups"][0].update(unknown=1),
        ]
        for mutation in mutations:
            document = self.example()
            mutation(document)
            with self.assertRaises((ValueError, RFModelError)):
                analyze_conversion_network(self.library, document)

    def test_explicit_global_noise_json_and_unchanged_carrier(self):
        document = self.example()
        del document["phase_noise_groups"]
        baseline = analyze_conversion_network(self.library, document)
        n = len(baseline["channels"])
        c = zero(n)
        for i in [0, 4, 6, 10]:
            c[i][i] = 1e-10
        document["additional_source_noise"] = {"covariance": [[v.real for v in row] for row in c]}
        result = analyze_conversion_network(self.library, document)
        self.close(self.output_noise(document)[0], 1e-10, 1e-10)
        for actual, expected in zip(result["channels"], baseline["channels"]):
            self.assertEqual(actual["outgoing"], expected["outgoing"])

    def test_cli_failure_preserves_result(self):
        with tempfile.TemporaryDirectory() as folder:
            source, output = Path(folder) / "source.json", Path(folder) / "output.json"
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
            document["phase_noise_groups"][0]["carriers"][0]["phase_gain"] = "bad"
            source.write_text(json.dumps(document))
            self.assertNotEqual(subprocess.run(command, capture_output=True, env=env).returncode, 0)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()

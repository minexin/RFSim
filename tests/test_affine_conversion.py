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


class AffineConversionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def close(self, actual, expected, scale=1):
        self.assertLessEqual(abs(actual - expected), 4e-12 * max(scale, abs(expected)))

    def test_widely_linear_complex_feedback_closed_form_and_noise_invariance(self):
        a, b, g, s, d = 0.3 + 0.1j, 0.12 - 0.04j, 0.2 + 0.1j, 0.4 - 0.2j, -0.3 + 0.5j
        device = dict(
            channels=[(0, 1)],
            direct=[[a]],
            conjugate=[[b]],
            reflection=[g],
            source=[s],
            source_covariance=[[1e-9]],
            source_complementary=[[0.2e-9j]],
            intrinsic_covariance=[[2e-10]],
            intrinsic_complementary=[[1e-10]],
        )
        options = dict(loaded_noise=True, additional_source_covariance=[[3e-10]])
        baseline = self.library.conversion_network(1, [device], **options)
        result = self.library.conversion_network(1, [device], output_offset=[d], **options)
        u, q = 1 - a * g, b * g.conjugate()
        z = a * s + b * s.conjugate() + d
        expected = (u.conjugate() * z + q * z.conjugate()) / (abs(u) ** 2 - abs(q) ** 2)
        self.close(result.outgoing[0], expected)
        self.close(result.incident[0], s + g * expected)
        for field in [
            "noise_covariance",
            "noise_complementary",
            "incident_noise_covariance",
            "incident_noise_complementary",
            "incident_outgoing_noise_covariance",
            "incident_outgoing_noise_complementary",
            "net_noise_into_device_w_per_hz",
        ]:
            self.assertEqual(getattr(result, field), getattr(baseline, field))

    def test_single_model_wrapper_and_zero_offset_equivalence(self):
        channels = [(0, 1)]
        baseline = self.library.frequency_conversion(1, channels, [[0.5]], source=[1])
        zero = self.library.frequency_conversion(
            1, channels, [[0.5]], source=[1], output_offset=[0]
        )
        self.assertEqual(baseline, zero)
        result = self.library.frequency_conversion(
            1, channels, [[0.5]], source=[1], output_offset=[0.2j]
        )
        self.close(result.outgoing[0], 0.5 + 0.2j)

    def test_emission_at_wired_port_propagates_and_is_not_boundary_drive(self):
        devices = [dict(channels=[(0, 1)], direct=[[0]]), dict(channels=[(0, 1)], direct=[[0.5]])]
        result = self.library.conversion_network(
            1, devices, [((0, 0), (1, 0))], output_offset=[1, 0]
        )
        self.assertEqual(result.outgoing, (1 + 0j, 0.5 + 0j))
        self.assertEqual(result.incident, (0.5 + 0j, 1 + 0j))

    def test_mixer_offset_removes_double_count_and_does_not_change_noise(self):
        channels = [(0, 12), (1, 10), (2, 2), (2, 22)]
        operating = [1, 2, 0, 0]
        lin = self.library.linearize_real_mixer(1, channels, operating, lo_bin=10)
        self.assertEqual(len(lin), 3)
        result = self.library.frequency_conversion(
            1,
            channels,
            lin.direct,
            lin.conjugate,
            source=operating,
            output_offset=lin.output_offset,
        )
        self.assertEqual(result.outgoing, lin.operating_outgoing)
        uncorrected = self.library.frequency_conversion(
            1, channels, lin.direct, lin.conjugate, source=operating
        )
        self.assertEqual(uncorrected.outgoing, tuple(2 * x for x in lin.operating_outgoing))

    def test_invalid_offsets_and_singular_feedback(self):
        for offset in [[], [0, 0], [complex(math.nan, 0)], [complex(0, math.inf)]]:
            with self.assertRaises((ValueError, RFModelError)):
                self.library.frequency_conversion(1, [(0, 1)], [[0.5]], output_offset=offset)
        with self.assertRaises(RFModelError):
            self.library.frequency_conversion(1, [(0, 0)], [[0.5]], output_offset=[1j])
        self.close(
            self.library.frequency_conversion(1, [(0, 0)], [[0.5]], output_offset=[2]).outgoing[0],
            2,
        )
        with self.assertRaises(RFModelError):
            self.library.frequency_conversion(1, [(0, 1)], [[1]], reflection=[1], output_offset=[1])

    def example(self):
        return json.loads((ROOT / "examples/affine-mixer-network.json").read_text())

    def row(self, result, name, port, bin_index):
        return next(
            (i, row)
            for i, row in enumerate(result["channels"])
            if (row["device"], row["port"], row["bin"]) == (name, port, bin_index)
        )

    def test_physical_network_nominal_point_and_shared_phase_noise(self):
        result = analyze_conversion_network(self.library, self.example())
        self.assertEqual(result["wave_relation"], "affine")
        self.assertEqual(result["operating_point_checks"][0]["device"], "mixer")
        self.assertTrue(result["operating_point_checks"][0]["passed"])
        for b in [2, 22]:
            _, row = self.row(result, "if_filter", 1, b)
            self.close(complex(*row["outgoing"]), 0.125)
        for b, noise in [(1, 0), (3, 0), (21, 6.25e-12), (23, 6.25e-12)]:
            i, _ = self.row(result, "if_filter", 1, b)
            self.close(complex(*result["noise_covariance_w_per_hz"][i][i]), noise, 1e-10)

    def test_inconsistent_working_point_is_rejected_instead_of_using_wrong_noise_jacobian(self):
        document = self.example()
        document["devices"][1]["model"]["operating_incident"][1] = 0.6
        with self.assertRaisesRegex(ValueError, "operating point inconsistent"):
            analyze_conversion_network(self.library, document)
        document = self.example()
        for boundary in document["boundaries"]:
            if boundary["channel"] == ["mixer", 1, 10]:
                boundary["source"] = 0.9
        with self.assertRaisesRegex(ValueError, "operating point inconsistent"):
            analyze_conversion_network(self.library, document)

    def test_reflected_if_incident_wave_is_part_of_supplied_point(self):
        document = self.example()
        for boundary in document["boundaries"]:
            if boundary["channel"] in [["if_filter", 1, 2], ["if_filter", 1, 22]]:
                boundary["reflection"] = 0.2
        with self.assertRaisesRegex(ValueError, "operating point inconsistent"):
            analyze_conversion_network(self.library, document)
        document["devices"][1]["model"]["operating_incident"][7] = 0.00625
        document["devices"][1]["model"]["operating_incident"][10] = 0.00625
        result = analyze_conversion_network(self.library, document)
        self.assertTrue(result["operating_point_checks"][0]["passed"])

    def test_explicit_device_offsets_and_single_conversion_json(self):
        device = dict(
            id="one",
            channels=[dict(port=0, bin=1)],
            model=dict(type="matrix", direct=[[0.5]]),
            noise=dict(noiseless=True),
            output_offset=[[0.2, 0.1]],
        )
        document = dict(
            format="rfmodel.conversion-network",
            version=1,
            spacing_hz=1,
            devices=[device],
            boundaries=[dict(channel=["one", 0, 1], source=1, reflection=0.2)],
        )
        result = analyze_conversion_network(self.library, document)
        self.close(complex(*result["channels"][0]["outgoing"]), (0.7 + 0.1j) / 0.9)
        single = dict(
            format="rfmodel.frequency-conversion",
            version=1,
            spacing_hz=1,
            channels=device["channels"],
            model=device["model"],
            source=[1],
            reflection=[0.2],
            output_offset=device["output_offset"],
        )
        scalar = analyze_conversion(self.library, single)
        self.close(complex(*scalar["channels"][0]["outgoing"]), (0.7 + 0.1j) / 0.9)
        linear = copy.deepcopy(document)
        linear["devices"][0] = dict(
            id="one",
            bins=[1],
            model=dict(type="linear", s=[[0.5]]),
            noise=dict(noiseless=True),
            output_offset=[[0.2, 0.1]],
        )
        self.close(
            complex(*analyze_conversion_network(self.library, linear)["channels"][0]["outgoing"]),
            (0.7 + 0.1j) / 0.9,
        )

    def test_json_rejects_offset_override_unknown_fields_and_booleans(self):
        mutations = [
            lambda d: d["devices"][1].update(output_offset=[0] * 12),
            lambda d: d["devices"][0].update(output_offset=[0]),
            lambda d: d["devices"][1]["model"].update(lo_bin=True),
            lambda d: d["devices"][1]["model"].update(operating_incident=[False] * 12),
            lambda d: d["devices"][1]["model"].update(extra=1),
        ]
        for mutation in mutations:
            document = self.example()
            mutation(document)
            with self.assertRaises((ValueError, RFModelError)):
                analyze_conversion_network(self.library, document)

    def test_cli_bad_working_point_preserves_existing_result(self):
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
            document["devices"][1]["model"]["operating_incident"][1] = 0.6
            source.write_text(json.dumps(document))
            self.assertNotEqual(subprocess.run(command, capture_output=True, env=env).returncode, 0)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()

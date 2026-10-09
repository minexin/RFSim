"""Conversion matrix, conjugate noise, feedback and JSON/CLI regression."""

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
from rfmodel.conversion_file import analyze_conversion
from rfmodel.model_file import load


def zero(n):
    return [[0j for _ in range(n)] for _ in range(n)]


class ConversionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def test_two_sideband_noise_folds_into_if(self):
        document = load(ROOT / "examples/mixer-image-noise.json")
        result = analyze_conversion(self.library, document)
        self.assertEqual(result["channels"][2]["outgoing"], [1, 0])
        self.assertAlmostEqual(complex(*result["noise_covariance_w_per_hz"][2][2]).real / 1e-20, 2)
        # The image and desired RF bands are independent; removing the image halves IF noise.
        document["source_noise"]["covariance"][0][0] = 0
        changed = analyze_conversion(self.library, document)
        self.assertAlmostEqual(complex(*changed["noise_covariance_w_per_hz"][2][2]).real / 1e-20, 1)

    def test_folded_sidebands_have_complementary_correlation(self):
        channels = [(0, 8), (1, 2), (1, 18)]
        phase = 0.4
        a, b = self.library.ideal_mixer_conversion(1.0, channels, lo_bin=10, phase_radians=phase)
        covariance = zero(3)
        covariance[0][0] = 1e-20
        result = self.library.frequency_conversion(
            1.0, channels, a, b, source_covariance=covariance
        )
        self.assertLess(abs(result.noise_covariance[1][2]), 1e-32)
        self.assertAlmostEqual(result.noise_complementary[1][2] / 1e-20, cmath.exp(2j * phase))
        self.assertAlmostEqual(result.noise_covariance[1][1] / 1e-20, 1)
        self.assertAlmostEqual(result.noise_covariance[2][2] / 1e-20, 1)

    def test_mixer_signal_matches_existing_including_dc(self):
        channels = [
            (0, 0),
            (0, 8),
            (0, 10),
            (0, 12),
            (1, 0),
            (1, 2),
            (1, 10),
            (1, 18),
            (1, 20),
            (1, 22),
        ]
        source = [0.3, 1 + 2j, 0.7 - 0.3j, -0.5 + 0.9j] + [0] * 6
        a, b = self.library.ideal_mixer_conversion(
            1e8, channels, lo_bin=10, gain_db=-3, phase_radians=0.4
        )
        result = self.library.frequency_conversion(1e8, channels, a, b, source=source)
        expected = self.library.ideal_mixer(
            1e8,
            {channels[i][1]: source[i] for i in range(4)},
            lo_bin=10,
            conversion_gain_db=-3,
            lo_phase_radians=0.4,
        )
        for i in range(4, len(channels)):
            self.assertAlmostEqual(result.outgoing[i], expected[channels[i][1]])
        self.assertEqual(result.outgoing[4].imag, 0)

    def test_dc_noise_and_upconverted_real_noise(self):
        channels = [(0, 10), (1, 0), (1, 20)]
        a, b = self.library.ideal_mixer_conversion(1.0, channels, lo_bin=10)
        c = zero(3)
        c[0][0] = 2
        result = self.library.frequency_conversion(1.0, channels, a, b, source_covariance=c)
        self.assertAlmostEqual(result.noise_covariance[1][1], 2)
        self.assertAlmostEqual(result.noise_complementary[1][1], 2)
        channels = [(0, 0), (1, 10)]
        a, b = self.library.ideal_mixer_conversion(1.0, channels, lo_bin=10, phase_radians=0.3)
        c = zero(2)
        c[0][0] = 1
        result = self.library.frequency_conversion(
            1.0, channels, a, b, source_covariance=c, source_complementary=c
        )
        self.assertAlmostEqual(result.noise_covariance[1][1], 2)
        self.assertAlmostEqual(result.noise_complementary[1][1], 2 * cmath.exp(0.6j))
        with self.assertRaises(RFModelError):
            self.library.frequency_conversion(1.0, channels, a, b, source_covariance=c)

    def test_complex_reflection_and_intrinsic_noise_against_scalar_formula(self):
        direct = 0.2 + 0.1j
        conjugate = -0.1 + 0.05j
        reflection = 0.3 - 0.2j
        source = 2 + 3j
        u = direct * reflection
        v = conjugate * reflection.conjugate()
        denominator = abs(1 - u) ** 2 - abs(v) ** 2

        def solve(drive):
            return ((1 - u.conjugate()) * drive + v * drive.conjugate()) / denominator

        expected = solve(direct * source + conjugate * source.conjugate())
        source_bases = [1 + 0.2j, 0.3 + 0.8j]
        intrinsic_bases = [0.7 + 0.1j, -0.2 + 0.4j]
        c_source = sum(abs(x) ** 2 for x in source_bases)
        p_source = sum(x * x for x in source_bases)
        c_intrinsic = sum(abs(x) ** 2 for x in intrinsic_bases)
        p_intrinsic = sum(x * x for x in intrinsic_bases)
        basis_outputs = [solve(direct * x + conjugate * x.conjugate()) for x in source_bases]
        basis_outputs += [solve(x) for x in intrinsic_bases]
        result = self.library.frequency_conversion(
            1.0,
            [(0, 1)],
            [[direct]],
            [[conjugate]],
            source=[source],
            reflection=[reflection],
            source_covariance=[[c_source]],
            source_complementary=[[p_source]],
            intrinsic_covariance=[[c_intrinsic]],
            intrinsic_complementary=[[p_intrinsic]],
        )
        self.assertAlmostEqual(result.outgoing[0], expected)
        self.assertAlmostEqual(result.incident[0], reflection * expected + source)
        self.assertAlmostEqual(
            result.noise_covariance[0][0], sum(abs(x) ** 2 for x in basis_outputs)
        )
        self.assertAlmostEqual(result.noise_complementary[0][0], sum(x * x for x in basis_outputs))
        self.assertLess(result.relative_residual, 1e-14)

    def test_bidirectional_conversion_feedback_across_frequencies(self):
        forward = 0.7 + 0.2j
        reverse = 0.1 - 0.15j
        source_reflection = 0.3 + 0.1j
        load_reflection = -0.2 + 0.25j
        denominator = 1 - forward * source_reflection * reverse * load_reflection
        result = self.library.frequency_conversion(
            1e8,
            [(0, 12), (1, 2)],
            [[0, reverse], [forward, 0]],
            source=[1, 0],
            reflection=[source_reflection, load_reflection],
            source_covariance=[[1e-20, 0], [0, 0]],
        )
        self.assertAlmostEqual(result.outgoing[1], forward / denominator)
        self.assertAlmostEqual(
            result.outgoing[0], reverse * load_reflection * forward / denominator
        )
        self.assertAlmostEqual(
            result.noise_covariance[1][1] / 1e-20, abs(forward / denominator) ** 2
        )

    def test_complex_linear_special_case(self):
        a = [[0.1, 0.2j], [0.3 + 0.4j, -0.1j]]
        c = [[2, 0], [0, 3]]
        result = self.library.frequency_conversion(1.0, [(0, 1), (1, 2)], a, source_covariance=c)
        expected = [
            [sum(a[i][k] * c[k][k] * a[j][k].conjugate() for k in range(2)) for j in range(2)]
            for i in range(2)
        ]
        for i in range(2):
            for j in range(2):
                self.assertAlmostEqual(result.noise_covariance[i][j], expected[i][j])
                self.assertLess(abs(result.noise_complementary[i][j]), 1e-14)

    def test_invalid_covariances_and_singular_feedback(self):
        for covariance, complementary in (([[1]], [[2]]), ([[-1]], [[0]]), ([[1j]], [[0]])):
            with self.assertRaises(RFModelError):
                self.library.frequency_conversion(
                    1.0,
                    [(0, 1)],
                    [[1]],
                    source_covariance=covariance,
                    source_complementary=complementary,
                )
        with self.assertRaises(RFModelError):
            self.library.frequency_conversion(1.0, [(0, 1)], [[1]], source=[1], reflection=[1])
        with self.assertRaises(RFModelError):
            self.library.frequency_conversion(1.0, [(0, 0)], [[1]], source=[1j])
        with self.assertRaises(RFModelError):
            self.library.frequency_conversion(1.0, [(0, 0)], [[1j]])

    def test_channel_and_matrix_validation(self):
        for channels in ([], [(0, 1), (0, 1)], [(True, 1)], [(0, -1)], [(0, 1.5)]):
            with self.assertRaises((ValueError, TypeError, RFModelError)):
                self.library.frequency_conversion(1.0, channels, [[1]])
        with self.assertRaises(RFModelError):
            self.library.frequency_conversion(1.0, [(0, 1), (0, 1)], zero(2))
        for field, value in (
            ("source", [1, 2]),
            ("source_covariance", [[1, 0], [0, 1]]),
            ("reflection", [math.nan]),
        ):
            with self.assertRaises((ValueError, RFModelError)):
                self.library.frequency_conversion(1.0, [(0, 1)], [[1]], **{field: value})
        with self.assertRaises(RFModelError):
            self.library.ideal_mixer_conversion(1.0, [(0, 8), (1, 2)], lo_bin=10)
        with self.assertRaises(RFModelError):
            self.library.frequency_conversion(0.0, [(0, 1)], [[1]])

    def test_json_matrix_and_strict_fields(self):
        document = {
            "format": "rfmodel.frequency-conversion",
            "version": 1,
            "spacing_hz": 1e9,
            "channels": [{"port": 0, "bin": 1}],
            "model": {"type": "matrix", "direct": [[0.2]], "conjugate": [[0.1]]},
            "source": [[2, 3]],
            "reflection": [0.5],
            "source_noise": {"covariance": [[2]]},
        }
        result = analyze_conversion(self.library, document)
        self.assertAlmostEqual(
            complex(*result["channels"][0]["outgoing"]), complex(0.6 / 0.85, 0.3 / 0.95)
        )
        for key, value in (("unknown", 1), ("version", True), ("source", None)):
            with self.assertRaises((ValueError, TypeError)):
                analyze_conversion(self.library, dict(document, **{key: value}))

    def test_cli_failure_preserves_output(self):
        document = load(ROOT / "examples/mixer-image-noise.json")
        environment = dict(os.environ)
        environment["PYTHONPATH"] = str(Path(rfmodel.__file__).resolve().parents[1])
        with tempfile.TemporaryDirectory() as directory:
            model = Path(directory) / "model.json"
            output = Path(directory) / "result.json"
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
            document["model"]["lo_bin"] = 0
            model.write_text(json.dumps(document), encoding="utf-8")
            run = subprocess.run(command, env=environment, capture_output=True, text=True)
            self.assertEqual(run.returncode, 1)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()

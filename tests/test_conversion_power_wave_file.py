"""Explicit-reference JSON/CLI checks against independent V/I and real-noise bases."""

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
from rfmodel.analysis_file import analyze_document
from rfmodel.model_file import _encode, _matrix, load

KT = 1.380649e-23 * 290
REFS = [31 + 12j, 73 - 21j, 19 + 8j, 110 + 31j, 43 - 12j, 22 + 7j, 91 - 18j]
SOURCE = .01 + .005j
PHASE = .4


def pair(value):
    value = complex(value)
    return [value.real, value.imag]


def zero(count):
    return [[0j] * count for _ in range(count)]


def boundary(reference, source=0j, resistance=50):
    """Independent Thevenin V+R*I=2*sqrt(R)*source, current into device."""
    reference = complex(reference)
    denominator = resistance + reference.conjugate()
    gain = 2 * math.sqrt(resistance * reference.real) / denominator
    return gain * source, (resistance - reference) / denominator, gain


def change_waves(incident, outgoing, references, old=50):
    """Definitions a=(V+ZI)/(2sqrt(ReZ)), b=(V-conj(Z)I)/(2sqrt(ReZ))."""
    a, b = [], []
    for x, y, z in zip(incident, outgoing, references):
        z = complex(z)
        voltage, current = math.sqrt(old) * (x + y), (x - y) / math.sqrt(old)
        a.append((voltage + z * current) / (2 * math.sqrt(z.real)))
        b.append((voltage - z.conjugate() * current) / (2 * math.sqrt(z.real)))
    return a, b


def example(scale=1):
    references = [scale * z for z in REFS]
    source, reflection, gain = boundary(references[0], SOURCE)
    c = zero(3)
    c[0][0] = abs(gain) ** 2 * 1e-20
    boundaries = [dict(channel=["mixer", 0, 8], source=pair(source), reflection=pair(reflection))]
    for index, bin_index in ((4, 2), (6, 18)):
        _, gamma, _ = boundary(references[index])
        boundaries.append(dict(channel=["resistor", 1, bin_index], reflection=pair(gamma)))
    return {
        "format": "rfmodel.conversion-network", "version": 1, "spacing_hz": 1e8,
        "reference_ohms": 50,
        "devices": [
            {"id": "mixer", "channels": [{"port": 0, "bin": 8}, {"port": 1, "bin": 2},
                                             {"port": 1, "bin": 18}],
             "model": {"type": "ideal_real_mixer", "lo_bin": 10, "phase_radians": PHASE},
             "reference_impedances_ohms": [pair(z) for z in references[:3]],
             "noise": {"noiseless": True}, "source_noise": {"covariance": _encode(c)}},
            {"id": "resistor", "bins": [2, 18],
             "model": {"type": "linear", "device": {"type": "resistor", "connection": "series",
                                                            "resistance_ohms": 25}},
             "reference_impedances_ohms": [pair(z) for z in references[3:]],
             "noise": {"temperature_k": 290}},
        ],
        "connections": [[["mixer", 1], ["resistor", 0]]], "boundaries": boundaries,
        "loaded_noise": True,
        "noise_analyses": [{"name": "lower_sideband", "reference_channels": [["mixer", 0, 8]],
                            "thermal_channels": [["mixer", 0, 8]],
                            "output_channel": ["resistor", 1, 2]}],
    }


def nominal_waves(source=0j, resistor_lower=0j, resistor_upper=0j):
    """50-ohm real mixer and 25-ohm series resistor equations, no RFModel helpers."""
    pump = cmath.exp(1j * PHASE)
    lower, upper = pump * source.conjugate(), pump * source
    a = [source, .2 * lower + resistor_lower, .2 * upper + resistor_upper,
         lower, 0j, upper, 0j]
    b = [0j, lower, upper, .2 * lower + resistor_lower, .8 * lower - resistor_lower,
         .2 * upper + resistor_upper, .8 * upper - resistor_upper]
    return a, b


def real_noise_bases():
    bases = []
    for component in (1, 1j):
        bases.append(nominal_waves(source=component * math.sqrt(1e-20 / 2)))
        bases.append(nominal_waves(resistor_lower=component * math.sqrt(.32 * KT / 2)))
        bases.append(nominal_waves(resistor_upper=component * math.sqrt(.32 * KT / 2)))
    return bases


class ConversionPowerWaveFileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = rfmodel.Library(LIBRARY_PATH)

    def near(self, actual, expected, scale=1):
        self.assertLessEqual(abs(actual - expected), 4e-12 * max(scale, abs(actual), abs(expected)),
                             (actual, expected))

    def matrix_near(self, actual, expected, scale=1):
        self.assertEqual(len(actual), len(expected))
        for row, target in zip(actual, expected):
            self.assertEqual(len(row), len(target))
            for a, b in zip(row, target):
                self.near(a, b, scale)

    def analyze(self, document, directory=None):
        return analyze_document(self.lib, document, base_directory=directory)

    def check_example(self, result, scale=1):
        refs = [scale * z for z in REFS]
        self.assertNotIn("reference_ohms", result)
        self.assertEqual(result["construction_reference_ohms"], 50)
        self.assertEqual(result["reference_impedances_ohms"], [pair(z) for z in refs])
        expected_a, expected_b = change_waves(*nominal_waves(SOURCE), refs)
        voltage, current = [], []
        for index, (channel, z) in enumerate(zip(result["channels"], refs)):
            self.assertEqual(channel["reference_impedance_ohms"], pair(z))
            a, b = complex(*channel["incident"]), complex(*channel["outgoing"])
            self.near(a, expected_a[index]); self.near(b, expected_b[index])
            voltage.append((z.conjugate() * a + z * b) / math.sqrt(z.real))
            current.append((a - b) / math.sqrt(z.real))
            self.near(abs(a)**2 - abs(b)**2, (voltage[-1] * current[-1].conjugate()).real)
        for first, second in ((1, 3), (2, 5)):
            self.near(voltage[first], voltage[second]); self.near(current[first], -current[second])
        for first, second in ((3, 4), (5, 6)):
            self.near(voltage[first] - voltage[second], 25 * current[first])
            self.near(current[first], -current[second])
        bases = [change_waves(a, b, refs) for a, b in real_noise_bases()]
        for field, side_a, side_b, conjugate in (
            ("noise_covariance_w_per_hz", 1, 1, True),
            ("noise_complementary_w_per_hz", 1, 1, False),
            ("incident_noise_covariance_w_per_hz", 0, 0, True),
            ("incident_noise_complementary_w_per_hz", 0, 0, False),
            ("incident_outgoing_noise_covariance_w_per_hz", 0, 1, True),
            ("incident_outgoing_noise_complementary_w_per_hz", 0, 1, False),
        ):
            expected = [[sum(x[side_a][i] * (x[side_b][j].conjugate() if conjugate else x[side_b][j])
                             for x in bases) for j in range(7)] for i in range(7)]
            self.matrix_near(_matrix(result[field]), expected, 1e-20)
        for i, channel in enumerate(result["channels"]):
            net = sum(abs(a[i])**2 - abs(b[i])**2 for a, b in bases)
            self.near(channel["net_noise_into_device_w_per_hz"], net, 1e-20)
        analysis = result["noise_analyses"][0]
        self.near(analysis["reference_gain"], .64)
        self.near(analysis["noise_factor"], 1.5)
        self.near(analysis["output_noise_w_per_hz"], .96 * KT, KT)
        self.near(analysis["equivalent_input_temperature_k"], 145)
        self.assertLess(result["relative_residual"], 1e-12)

    def test_mixer_resistor_full_physical_waves_and_noise(self):
        for scale in (.5, 1, 2):
            with self.subTest(scale=scale):
                document = example(scale); saved = copy.deepcopy(document)
                result = self.analyze(document)
                self.assertEqual(document, saved)
                self.check_example(result, scale)

    def test_example_json_reproduces_independent_physics(self):
        document = load(ROOT / "examples/mixed-reference-conversion.json")
        self.assertEqual(document, example())
        self.check_example(self.analyze(document))

    def test_single_parameter_mixer_transforms_signal_and_preserves_physical_boundary(self):
        device = example()["devices"][0]
        refs = REFS[:3]
        boundaries = [boundary(z, SOURCE if i == 0 else 0) for i,z in enumerate(refs)]
        document = {"format": "rfmodel.frequency-conversion", "version": 1, "spacing_hz": 1e8,
                    "channels": device["channels"], "model": device["model"],
                    "reference_impedances_ohms": device["reference_impedances_ohms"],
                    "source": [pair(x[0]) for x in boundaries],
                    "reflection": [pair(x[1]) for x in boundaries],
                    "source_noise": device["source_noise"]}
        result = self.analyze(document)
        pump = cmath.exp(1j*PHASE)
        a, b = change_waves([SOURCE,0,0], [0,pump*SOURCE.conjugate(),pump*SOURCE], refs)
        for i,row in enumerate(result["channels"]):
            self.near(complex(*row["incident"]), a[i])
            self.near(complex(*row["outgoing"]), b[i])
        gains = [x[2] for x in boundaries]
        c, p = _matrix(result["noise_covariance_w_per_hz"]), _matrix(result["noise_complementary_w_per_hz"])
        for i in (1,2):
            self.near(abs(gains[i])**2*c[i][i], 1e-20, 1e-20)
        self.near(gains[1]*gains[2]*p[1][2], pump**2*1e-20, 1e-20)
        self.near(c[1][2], 0, 1e-20)

    def test_single_matrix_affine_and_explicit_c_p_are_already_local(self):
        a, b, gamma, source, offset = .2 + .1j, -.1 + .05j, .3 - .2j, 2 + 3j, .12 - .2j
        source_bases, intrinsic_bases = [1 + .2j, .3 + .8j], [.7 + .1j, -.2 + .4j]
        def covariance(bases):
            return {"covariance": [[sum(abs(x)**2 for x in bases)]],
                    "complementary": [[pair(sum(x*x for x in bases))]]}
        doc = {"format": "rfmodel.frequency-conversion", "version": 1, "spacing_hz": 1,
               "channels": [{"port": 0, "bin": 1}], "reference_impedances_ohms": [[31, 12]],
               "model": {"type": "matrix", "direct": [[pair(a)]], "conjugate": [[pair(b)]]},
               "source": [pair(source)], "reflection": [pair(gamma)], "output_offset": [pair(offset)],
               "source_noise": covariance(source_bases), "intrinsic_noise": covariance(intrinsic_bases)}
        result = self.analyze(doc)
        u, v = a * gamma, b * gamma.conjugate()
        def solve(drive):
            return ((1-u.conjugate())*drive + v*drive.conjugate()) / (abs(1-u)**2-abs(v)**2)
        expected = solve(a*source+b*source.conjugate()+offset)
        self.near(complex(*result["channels"][0]["outgoing"]), expected)
        outputs = [solve(a*x+b*x.conjugate()) for x in source_bases] + [solve(x) for x in intrinsic_bases]
        self.near(complex(*result["noise_covariance_w_per_hz"][0][0]), sum(abs(x)**2 for x in outputs))
        self.near(complex(*result["noise_complementary_w_per_hz"][0][0]), sum(x*x for x in outputs))

    def test_parameter_mixer_explicit_noise_and_offsets_stay_local(self):
        vector = [.2 + .1j, -.3 + .2j, .1 - .4j]
        c = [[x*y.conjugate() for y in vector] for x in vector]
        p = [[x*y for y in vector] for x in vector]
        device = example()["devices"][0]
        device.pop("source_noise")
        document = {"format": "rfmodel.frequency-conversion", "version": 1, "spacing_hz": 1e8,
                    "channels": device["channels"], "model": device["model"],
                    "reference_impedances_ohms": device["reference_impedances_ohms"],
                    "intrinsic_noise": {"covariance": _encode(c), "complementary": _encode(p)},
                    "output_offset": [pair(x) for x in vector]}
        single = self.analyze(document)
        device["noise"] = document["intrinsic_noise"]
        device["output_offset"] = document["output_offset"]
        network = self.analyze({"format": "rfmodel.conversion-network", "version": 1, "spacing_hz": 1e8,
                                "devices": [device], "boundaries": [
                                    {"channel": ["mixer", row["port"], row["bin"]]} for row in device["channels"]]})
        for result in (single, network):
            self.matrix_near(_matrix(result["noise_covariance_w_per_hz"]), c)
            self.matrix_near(_matrix(result["noise_complementary_w_per_hz"]), p)
            for row, expected in zip(result["channels"], vector):
                self.near(complex(*row["outgoing"]), expected)

    def test_linear_static_matrix_is_local_parameter_model_transforms(self):
        z1, z2, resistance = 31 + 12j, 73 - 21j, 25
        d = resistance + z1 + z2
        t = 2 * math.sqrt(z1.real*z2.real) / d
        s = [[1-2*z1.real/d, t], [t, 1-2*z2.real/d]]
        c = [[2e-20, .5e-20j], [-.5e-20j, 3e-20]]
        base = {"format": "rfmodel.conversion-network", "version": 1, "spacing_hz": 1e9,
                "devices": [{"id": "r", "bins": [1], "model": {"type": "linear", "s": _encode(s)},
                             "reference_impedances_ohms": [pair(z1), pair(z2)],
                             "noise": {"covariance": _encode(c)}, "output_offset": [[.1, .2], [.3, -.1]]}],
                "boundaries": [{"channel": ["r", 0, 1], "source": [.2, -.1]},
                               {"channel": ["r", 1, 1], "source": [-.3, .2]}]}
        for parameter in (False, True):
            document = copy.deepcopy(base)
            if parameter:
                document["devices"][0]["model"] = {"type": "linear", "device": {
                    "type": "resistor", "connection": "series", "resistance_ohms": resistance}}
            result = self.analyze(document)
            source = [.2-.1j, -.3+.2j]; offset = [.1+.2j, .3-.1j]
            for i, row in enumerate(result["channels"]):
                self.near(complex(*row["outgoing"]), sum(s[i][j]*source[j] for j in range(2))+offset[i])
            self.matrix_near(_matrix(result["noise_covariance_w_per_hz"]), c, 1e-20)

    def test_dc_lift_noise_and_frequency_major_reference_order(self):
        refs = [31, 73, 19+8j, 110+31j]
        document = {"format": "rfmodel.conversion-network", "version": 1, "spacing_hz": 1e9,
                    "devices": [{"id": "r", "bins": [0, 1], "model": {"type": "linear", "device": {
                        "type": "resistor", "connection": "series", "resistance_ohms": 25}},
                        "reference_impedances_ohms": [pair(z) for z in refs], "noise": {"temperature_k": 290}}],
                    "boundaries": [{"channel": ["r", port, index]} for index in (0, 1) for port in (0, 1)]}
        result = self.analyze(document)
        c, p = _matrix(result["noise_covariance_w_per_hz"]), _matrix(result["noise_complementary_w_per_hz"])
        for sample in range(2):
            za, zb = [complex(z) for z in refs[2*sample:2*sample+2]]
            d = 25 + za + zb; t = 2*math.sqrt(za.real*zb.real)/d
            s = [[1-2*za.real/d, t], [t, 1-2*zb.real/d]]
            for i in range(2):
                for j in range(2):
                    expected = KT*((1 if i == j else 0)-sum(s[i][k]*s[j][k].conjugate() for k in range(2)))
                    self.near(c[2*sample+i][2*sample+j], expected, KT)
                    self.near(p[2*sample+i][2*sample+j], expected if sample == 0 else 0, KT)
        self.assertEqual([(row["bin"], row["port"]) for row in result["channels"]], [(0,0),(0,1),(1,0),(1,1)])

    def test_touchstone_embedded_noise_preserves_physical_loads(self):
        refs = [31+12j, 73-21j]
        document = {"format": "rfmodel.conversion-network", "version": 1, "spacing_hz": 2e9,
                    "devices": [{"id": "amp", "bins": [1], "model": {"type": "linear", "device": {
                        "type": "touchstone", "path": "noisy-amplifier.s2p"}},
                        "reference_impedances_ohms": [pair(z) for z in refs], "noise": {"touchstone": True}}],
                    "boundaries": [{"channel": ["amp", i, 1], "reflection": pair(boundary(z)[1])}
                                   for i,z in enumerate(refs)]}
        result = self.analyze(document, ROOT / "examples")
        c = _matrix(result["noise_covariance_w_per_hz"])
        # The independent fixture's 75-ohm noise transformed to physical 50-ohm terminations.
        common = [[.96*KT, -.384*KT], [-.384*KT, 3.9936*KT]]
        gain = [boundary(z)[2] for z in refs]
        for i in range(2):
            for j in range(2):
                self.near(gain[i]*c[i][j]*gain[j].conjugate(), common[i][j], KT)
        self.matrix_near(_matrix(result["noise_complementary_w_per_hz"]), zero(2), KT)

    def test_partial_vectors_report_default_refs_without_changing_local_matrices(self):
        document = {"format": "rfmodel.conversion-network", "version": 1, "spacing_hz": 1,
                    "reference_ohms": 75, "devices": [
                        {"id": "a", "channels": [{"port": 0, "bin": 1}],
                         "model": {"type": "matrix", "direct": [[.2]]}, "noise": {"noiseless": True},
                         "reference_impedances_ohms": [[31, 12]]},
                        {"id": "b", "channels": [{"port": 0, "bin": 1}],
                         "model": {"type": "matrix", "direct": [[.3]]}, "noise": {"noiseless": True}}],
                    "boundaries": [{"channel": [name, 0, 1], "source": 1} for name in ("a", "b")]}
        result = self.analyze(document)
        self.assertEqual(result["reference_impedances_ohms"], [[31,12],[75,0]])
        self.assertEqual(result["construction_reference_ohms"], 75)
        self.near(complex(*result["channels"][0]["outgoing"]), .2)
        self.near(complex(*result["channels"][1]["outgoing"]), .3)

    def test_global_correlated_source_noise_stays_local(self):
        coefficients = [.2+.1j, -.3+.05j]; vector = [.4+.2j, -.1+.3j]
        document = {"format": "rfmodel.conversion-network", "version": 1, "spacing_hz": 1,
                    "devices": [{"id": str(i), "channels": [{"port": 0, "bin": 1}],
                                 "model": {"type": "matrix", "direct": [[pair(coefficients[i])]]},
                                 "noise": {"noiseless": True}, "reference_impedances_ohms": [pair(REFS[i])]}
                                for i in range(2)],
                    "boundaries": [{"channel": [str(i), 0, 1]} for i in range(2)],
                    "additional_source_noise": {
                        "covariance": _encode([[x*y.conjugate() for y in vector] for x in vector]),
                        "complementary": _encode([[x*y for y in vector] for x in vector])}, "loaded_noise": True}
        result = self.analyze(document)
        waves = [a*b for a,b in zip(coefficients, vector)]
        self.matrix_near(_matrix(result["noise_covariance_w_per_hz"]), [[x*y.conjugate() for y in waves] for x in waves])
        self.matrix_near(_matrix(result["noise_complementary_w_per_hz"]), [[x*y for y in waves] for x in waves])
        self.matrix_near(_matrix(result["incident_outgoing_noise_complementary_w_per_hz"]),
                         [[x*y for y in waves] for x in vector])

    def test_legacy_output_shape_unchanged(self):
        single = {"format": "rfmodel.frequency-conversion", "version": 1, "spacing_hz": 1,
                  "channels": [{"port": 0, "bin": 1}], "model": {"type": "matrix", "direct": [[.2]]}, "source": [1]}
        network = {"format": "rfmodel.conversion-network", "version": 1, "spacing_hz": 1,
                   "devices": [{"id": "a", "channels": single["channels"], "model": single["model"],
                                "noise": {"noiseless": True}}], "boundaries": [{"channel": ["a",0,1], "source": 1}]}
        for base in (single, network):
            before = self.analyze(base); explicit = copy.deepcopy(base)
            (explicit if base is single else explicit["devices"][0])["reference_impedances_ohms"] = [50]
            after = self.analyze(explicit)
            after["reference_ohms"] = after.pop("construction_reference_ohms")
            after.pop("reference_impedances_ohms")
            for row in after["channels"]: row.pop("reference_impedance_ohms")
            self.assertEqual(before, after)
            self.assertNotIn("reference_impedances_ohms", before)

    def test_invalid_vectors_and_complex_dc_are_rejected(self):
        base = {"format": "rfmodel.frequency-conversion", "version": 1, "spacing_hz": 1,
                "channels": [{"port": 0, "bin": 1}], "model": {"type": "matrix", "direct": [[0]]}}
        for references in (None, {}, [], [50,75], [True], [0], [-1], [[0,2]], [[50,1,2]],
                           [float("nan")], [[50,float("inf")]], ["50"]):
            with self.subTest(references=references), self.assertRaises((ValueError, rfmodel.RFModelError)):
                self.analyze(dict(base, reference_impedances_ohms=references))
            device = {"id":"a", "channels":base["channels"], "model":base["model"],
                      "noise":{"noiseless":True}, "reference_impedances_ohms":references}
            with self.assertRaises((ValueError, rfmodel.RFModelError)):
                self.analyze({"format":"rfmodel.conversion-network","version":1,"spacing_hz":1,
                              "devices":[device],"boundaries":[{"channel":["a",0,1]}]})
        base["channels"][0]["bin"] = 0
        with self.assertRaisesRegex(ValueError, "DC"):
            self.analyze(dict(base, reference_impedances_ohms=[[50,1]]))
        for field in ("source", "reflection", "output_offset"):
            with self.assertRaises((ValueError, rfmodel.RFModelError)):
                self.analyze(dict(base, reference_impedances_ohms=[50], **{field: [[0,1]]}))

    def test_unsupported_automatic_paths_fail_explicitly(self):
        for field, value, message in (("frequency_plan", {}, "frequency_plan"),
                                      ("operating_point", {}, "operating_point"),
                                      ("phase_noise_sources", [], "phase_noise"),
                                      ("phase_noise_groups", [], "phase_noise")):
            document = example(); document[field] = value
            with self.assertRaisesRegex(ValueError, message): self.analyze(document)
        for kind in ("linearized_real_mixer", "bilinear_real_mixer", "saturating_amplifier",
                     "polynomial_amplifier", "highorder_amplifier"):
            document = example(); document["devices"][0]["model"] = {"type":kind}
            with self.assertRaisesRegex(ValueError, "operating-point"):
                self.analyze(document)

    def cli(self, model, output):
        environment = dict(os.environ, PYTHONPATH=str(Path(rfmodel.__file__).resolve().parents[1]))
        return subprocess.run([sys.executable, "-m", "rfmodel", str(model), "--library", LIBRARY_PATH,
                               "--output", str(output)], env=environment, capture_output=True, text=True)

    def test_cli_replay_and_invalid_reference_preserves_output(self):
        with tempfile.TemporaryDirectory() as directory:
            model, output = Path(directory)/"model.json", Path(directory)/"output.json"
            document = example(); model.write_text(json.dumps(document), encoding="utf-8")
            run = self.cli(model, output); self.assertEqual(run.returncode, 0, run.stderr)
            self.check_example(json.loads(output.read_text(encoding="utf-8")))
            saved = output.read_bytes()
            run = self.cli(model, output); self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(output.read_bytes(), saved)
            document["devices"][1]["reference_impedances_ohms"].pop()
            model.write_text(json.dumps(document), encoding="utf-8")
            run = self.cli(model, output); self.assertEqual(run.returncode, 1)
            self.assertIn("reference impedance", run.stderr)
            self.assertEqual(output.read_bytes(), saved)


if __name__ == "__main__":
    unittest.main()

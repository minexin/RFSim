"""Fixed-pump complex-reference bindings, checked against independent V/I equations."""
import copy
import math
from pathlib import Path
import sys
import unittest

LIBRARY_PATH = str(Path(sys.argv.pop(1)).resolve())
if "--installed" in sys.argv:
    sys.argv.remove("--installed")
else:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
import rfmodel


def zero(n):
    return [[0j] * n for _ in range(n)]


def covariance(left, right=None, complementary=False):
    right = left if right is None else right
    n = len(left[0])
    return [[sum(x[i] * (y[j] if complementary else y[j].conjugate())
                 for x, y in zip(left, right)) for j in range(n)] for i in range(n)]


def solve_real_linear(residual, n):
    """Independent real Gaussian elimination; residual is complex and affine."""
    def flatten(values):
        return [part for value in values for part in (value.real, value.imag)]

    origin = flatten(residual([0j] * n))
    columns = []
    for k in range(2 * n):
        value = [0j] * n
        value[k // 2] = 1j if k % 2 else 1
        columns.append([a - b for a, b in zip(flatten(residual(value)), origin)])
    rows = [[columns[j][i] for j in range(2 * n)] + [-origin[i]] for i in range(2 * n)]
    for col in range(2 * n):
        pivot = max(range(col, 2 * n), key=lambda row: abs(rows[row][col]))
        rows[col], rows[pivot] = rows[pivot], rows[col]
        divisor = rows[col][col]
        if abs(divisor) < 1e-14:
            raise AssertionError("Singular independent fixture")
        rows[col] = [x / divisor for x in rows[col]]
        for row in range(2 * n):
            if row != col:
                factor = rows[row][col]
                rows[row] = [a - factor * b for a, b in zip(rows[row], rows[col])]
    return [complex(rows[2 * i][-1], rows[2 * i + 1][-1]) for i in range(n)]


def currents(y, j, voltage, norton):
    return [sum(y[i][k] * voltage[k] + j[i][k] * voltage[k].conjugate()
                for k in range(len(voltage))) + norton[i] for i in range(len(voltage))]


def wave_coordinates(voltage, current, refs):
    incident = [(v + z * i) / (2 * math.sqrt(z.real)) for v, i, z in zip(voltage, current, refs)]
    outgoing = [(v - z.conjugate() * i) / (2 * math.sqrt(z.real)) for v, i, z in zip(voltage, current, refs)]
    return incident, outgoing


def local_model(y, j, refs, offset, noise):
    """Derive emitted waves from Norton equations without using RFModel transforms."""
    n = len(refs)

    def emitted(incident, norton):
        def residual(outgoing):
            voltage = [(z.conjugate() * a + z * b) / math.sqrt(z.real)
                       for a, b, z in zip(incident, outgoing, refs)]
            current = currents(y, j, voltage, norton)
            return [(a - b) / math.sqrt(z.real) - i
                    for a, b, z, i in zip(incident, outgoing, refs, current)]
        return solve_real_linear(residual, n)

    a, b = zero(n), zero(n)
    for k in range(n):
        real, imag = [0j] * n, [0j] * n
        real[k], imag[k] = 1, 1j
        br, bi = emitted(real, [0j] * n), emitted(imag, [0j] * n)
        for i in range(n):
            a[i][k] = (br[i] - 1j * bi[i]) / 2
            b[i][k] = (br[i] + 1j * bi[i]) / 2
    intrinsic = [emitted([0j] * n, basis) for basis in noise]
    return dict(direct=a, conjugate=b, output_offset=emitted([0j] * n, offset),
                intrinsic_covariance=covariance(intrinsic),
                intrinsic_complementary=covariance(intrinsic, complementary=True))


class ConversionPowerWaveApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = rfmodel.Library(LIBRARY_PATH)

    def near(self, actual, expected, scale=1):
        if isinstance(expected, (tuple, list)):
            self.assertEqual(len(actual), len(expected))
            for x, y in zip(actual, expected):
                self.near(x, y, scale)
        else:
            self.assertLessEqual(abs(actual - expected), 3e-11 * max(scale, abs(expected)))

    def test_default_tuple_compatibility_and_explicit_scalar_precedence(self):
        device = dict(channels=[(0, 1)], direct=[[.3 + .1j]], conjugate=[[.1j]],
                      source=[1 + .2j], source_covariance=[[2]], source_complementary=[[.2j]])
        for loaded in (False, True):
            old = self.lib.conversion_network(1, [device], loaded_noise=loaded)
            omitted = self.lib.conversion_network(1, [dict(device, reference_impedances_ohms=None)],
                                                  loaded_noise=loaded)
            explicit = self.lib.conversion_network(1, [dict(device, reference_impedances_ohms=[50])],
                                                   loaded_noise=loaded, reference_ohms=75)
            self.assertEqual(type(old), rfmodel.ConversionLoadedResult if loaded else rfmodel.ConversionResult)
            self.assertEqual(omitted, old)
            self.assertEqual(tuple(explicit)[:-1], tuple(old))
            self.assertEqual(explicit.reference_impedances_ohms, (50,))
        plain = self.lib.frequency_conversion(1, **device)
        explicit = self.lib.frequency_conversion(1, **device, reference_impedances_ohms=[50])
        self.assertEqual(tuple(explicit)[:-1], tuple(plain))
        affine = self.lib.frequency_conversion(1, **device, reference_impedances_ohms=[31 + 7j],
                                               output_offset=[.2j])
        self.near(affine.outgoing[0], plain.outgoing[0] + .2j)
        self.assertEqual(affine.reference_impedances_ohms, (31 + 7j,))
        fallback = self.lib.conversion_network(1, [dict(device, reference_impedances_ohms=[31 + 7j]),
                                                   dict(channels=[(0, 1)], direct=[[0]])],
                                               reference_ohms=73)
        self.assertEqual(fallback.reference_impedances_ohms, (31 + 7j, 73))

    def fixture(self):
        channels = [(0, 1), (0, 2), (1, 1)]
        y = [[.019 + .003j, -.001 + .0007j, .002j],
             [.0004 - .001j, .026 - .004j, -.002],
             [-.001j, .0008, .021 + .002j]]
        j = [[.001 + .0003j, .0004j, -.0002],
             [.0001, -.0008j, .0003 + .0002j],
             [.0005j, -.0003, .0007]]
        second_y = [[value * 1.3 for value in row] for row in y]
        second_j = [[value * -.6 for value in row] for row in j]
        offset = [[.003 + .002j, -.001j, .004], [-.002, .003j, .001 - .002j]]
        noise = [
            [[.001 + .0002j, -.0004j, .0005], [.0003j, .0007, -.0002 + .0001j]],
            [[-.0006j, .0008 + .0003j, .0004], [.0002, -.0001j, .0009j]],
        ]
        refs = [[31 + 12j, 73 - 21j, 44 + 5j], [82 - 11j, 23 + 8j, 67 - 17j]]
        source_impedance = {2: 61 + 13j, 5: 39 - 9j}
        voltage_source = {2: .4 + .2j, 5: -.1 + .07j}
        voltage_noise = [{2: .015 + .007j, 5: -.01j}, {2: .003j, 5: .012 + .004j}]
        return channels, [y, second_y], [j, second_j], offset, noise, refs, source_impedance, voltage_source, voltage_noise

    def physical_solution(self, ys, js, norton, source_impedance, source):
        def current(voltage):
            return sum((currents(y, j, voltage[3 * d:3 * d + 3], norton[d])
                        for d, (y, j) in enumerate(zip(ys, js))), [])

        def residual(voltage):
            i = current(voltage)
            return [voltage[0] - voltage[3], voltage[1] - voltage[4],
                    i[0] + i[3], i[1] + i[4],
                    voltage[2] + source_impedance[2] * i[2] - source.get(2, 0),
                    voltage[5] + source_impedance[5] * i[5] - source.get(5, 0)]
        voltage = solve_real_linear(residual, 6)
        return voltage, current(voltage)

    def test_independent_norton_network_full_loaded_cp_and_offsets(self):
        channels, ys, js, offset, noise, references, zs, vs, en = self.fixture()
        old = [50 + 0j] * 3
        devices, global_offset = [], []
        for d in range(2):
            before = local_model(ys[d], js[d], old, offset[d], noise[d])
            transformed = self.lib.renormalize_conversion_power_waves(
                1e6, channels, before['direct'], old, references[d], before['conjugate'],
                output_offset=before['output_offset'], intrinsic_covariance=before['intrinsic_covariance'],
                intrinsic_complementary=before['intrinsic_complementary'])
            independent = local_model(ys[d], js[d], references[d], offset[d], noise[d])
            for field in independent:
                self.near(getattr(transformed, field), independent[field], 1e-7 if 'covariance' in field or 'complementary' in field else 1)
            for basis in noise[d]:
                emitted_old = local_model(ys[d], js[d], old, basis, noise[d])['output_offset']
                emitted_new = local_model(ys[d], js[d], references[d], basis, noise[d])['output_offset']
                mapped = [sum(transformed.noise_direct[i][k] * emitted_old[k] +
                              transformed.noise_conjugate[i][k] * emitted_old[k].conjugate()
                              for k in range(3)) for i in range(3)]
                self.near(mapped, emitted_new)
            self.assertGreater(max(abs(v) for row in transformed.noise_conjugate for v in row), 1e-4)
            source, reflection = [0j] * 3, [0j] * 3
            global_index = 3 * d + 2
            z = references[d][2]
            source[2] = vs[global_index] * math.sqrt(z.real) / (zs[global_index] + z.conjugate())
            reflection[2] = (zs[global_index] - z) / (zs[global_index] + z.conjugate())
            devices.append(dict(channels=channels, direct=transformed.direct, conjugate=transformed.conjugate,
                                intrinsic_covariance=transformed.intrinsic_covariance,
                                intrinsic_complementary=transformed.intrinsic_complementary,
                                source=source, reflection=reflection,
                                reference_impedances_ohms=references[d]))
            global_offset.extend(transformed.output_offset)
        refs = sum(references, [])
        source_bases = [[e.get(i, 0) * math.sqrt(z.real) / (zs[i] + z.conjugate()) if i in zs else 0j
                         for i, z in enumerate(refs)] for e in en]
        result = self.lib.conversion_network(
            1e6, devices, [((0, 0), (1, 0))], loaded_noise=True, output_offset=global_offset,
            additional_source_covariance=covariance(source_bases),
            additional_source_complementary=covariance(source_bases, complementary=True))
        voltage, current = self.physical_solution(ys, js, offset, zs, vs)
        incident, outgoing = wave_coordinates(voltage, current, refs)
        self.near(result.incident, incident)
        self.near(result.outgoing, outgoing)
        self.assertEqual(result.reference_impedances_ohms, tuple(refs))
        ai, bo = [], []
        physical_noise = []
        for d in range(2):
            for basis in noise[d]:
                norton = [[0j] * 3, [0j] * 3]
                norton[d] = basis
                physical_noise.append(self.physical_solution(ys, js, norton, zs, {}))
        for e in en:
            physical_noise.append(self.physical_solution(ys, js, [[0j] * 3, [0j] * 3], zs, e))
        for v, i in physical_noise:
            a, b = wave_coordinates(v, i, refs)
            ai.append(a)
            bo.append(b)
        for name, first, second, complementary in (
            ('noise_covariance', bo, bo, False), ('noise_complementary', bo, bo, True),
            ('incident_noise_covariance', ai, ai, False), ('incident_noise_complementary', ai, ai, True),
            ('incident_outgoing_noise_covariance', ai, bo, False),
            ('incident_outgoing_noise_complementary', ai, bo, True)):
            self.near(getattr(result, name), covariance(first, second, complementary), 1e-4)
        expected_power = [sum((v[k] * i[k].conjugate()).real for v, i in physical_noise) for k in range(6)]
        self.near(result.net_noise_into_device_w_per_hz, expected_power, 1e-4)
        for first, second in ((0, 3), (1, 4)):
            self.near(voltage[first], voltage[second])
            self.near(current[first], -current[second])
            self.near(result.net_noise_into_device_w_per_hz[first],
                      -result.net_noise_into_device_w_per_hz[second], 1e-4)
        self.assertGreater(abs(result.noise_complementary[0][4]), 1e-10)

    def test_boundary_matches_thevenin_full_cp_and_retains_internal_zeros(self):
        channels = [(0, 1), (0, 2), (1, 3)]
        old, new = [50, 30 + 9j, 81 - 4j], [21 + 7j, 93 - 15j, 64 + 12j]
        zs, vs = [35 + 4j, 0, 74 - 5j], [.4 + .2j, 0, -.2j]
        basis = [[.2 + .05j, 0, -.1j], [.03j, 0, .04 - .06j]]
        def boundary(refs):
            l = [math.sqrt(complex(z).real) / (zs[i] + complex(z).conjugate()) if i != 1 else 0
                 for i, z in enumerate(refs)]
            waves = [[l[i] * v[i] for i in range(3)] for v in basis]
            return dict(source=[l[i] * vs[i] for i in range(3)],
                        reflection=[(zs[i] - z) / (zs[i] + complex(z).conjugate()) if i != 1 else 0
                                    for i, z in enumerate(refs)],
                        source_covariance=covariance(waves),
                        source_complementary=covariance(waves, complementary=True))
        original = boundary(old)
        saved = copy.deepcopy(original)
        changed = self.lib.renormalize_conversion_boundary(1, channels, old, new, external_channels=[0, 2], **original)
        self.assertEqual(original, saved)
        for field, expected in boundary(new).items():
            self.near(getattr(changed, field), expected)
        self.assertEqual(changed.source[1], 0)
        self.assertEqual(changed.reflection[1], 0)
        self.assertEqual(changed.source_covariance[1], (0, 0, 0))
        self.assertGreater(abs(changed.source_complementary[0][2]), 1e-6)
        restored = self.lib.renormalize_conversion_boundary(
            1, channels, new, old, external_channels=[0, 2],
            **{name: getattr(changed, name) for name in original})
        for field in original:
            self.near(getattr(restored, field), original[field])

    def test_dc_physical_round_trip_and_identity_representation(self):
        channels = [(0, 0), (0, 1)]
        direct, conjugate = [[.2, .05j], [.1 + .02j, .3]], [[.1, -.05j], [.03j, .04j]]
        offset, c, p = [.2, .1j], [[.04, .01j], [-.01j, .08]], [[.04, -.01j], [-.01j, .02]]
        identity = self.lib.renormalize_conversion_power_waves(
            1, channels, direct, [50, 50], [50, 50], conjugate,
            output_offset=offset, intrinsic_covariance=c, intrinsic_complementary=p)
        self.assertEqual(identity.direct, tuple(map(tuple, direct)))
        self.assertEqual(identity.conjugate, tuple(map(tuple, conjugate)))
        self.assertEqual(identity.noise_direct, ((1, 0), (0, 1)))
        transformed = self.lib.renormalize_conversion_power_waves(
            1, channels, direct, [50, 50], [23, 71 + 13j], conjugate,
            output_offset=offset, intrinsic_covariance=c, intrinsic_complementary=p)
        restored = self.lib.renormalize_conversion_power_waves(
            1, channels, transformed.direct, [23, 71 + 13j], [50, 50], transformed.conjugate,
            output_offset=transformed.output_offset, intrinsic_covariance=transformed.intrinsic_covariance,
            intrinsic_complementary=transformed.intrinsic_complementary)
        for source in ([.4, .1 + .3j], [-.2, -.7j]):
            before = self.lib.frequency_conversion(1, channels, direct, conjugate, source=source,
                                                   output_offset=offset, intrinsic_covariance=c,
                                                   intrinsic_complementary=p)
            after = self.lib.frequency_conversion(1, channels, restored.direct, restored.conjugate, source=source,
                                                  output_offset=restored.output_offset,
                                                  intrinsic_covariance=restored.intrinsic_covariance,
                                                  intrinsic_complementary=restored.intrinsic_complementary)
            self.near(after.outgoing, before.outgoing)
            self.near(after.noise_covariance, before.noise_covariance)
            self.near(after.noise_complementary, before.noise_complementary)

    def test_invalid_references_dimensions_dc_and_nonfinite(self):
        for refs in ([], [50, 50], [0], [-1], [float('nan')], [complex(50, math.inf)]):
            with self.subTest(refs=refs), self.assertRaises((ValueError, rfmodel.RFModelError)):
                self.lib.frequency_conversion(1, [(0, 1)], [[.2]], reference_impedances_ohms=refs)
        for old, new in (([50j], [50]), ([50], [-1]), ([50], [math.inf]), ([50], [])):
            with self.assertRaises((ValueError, rfmodel.RFModelError)):
                self.lib.renormalize_conversion_power_waves(1, [(0, 1)], [[.2]], old, new)
        with self.assertRaises(rfmodel.RFModelError):
            self.lib.frequency_conversion(1, [(0, 0)], [[.2]], reference_impedances_ohms=[50 + 1j])
        with self.assertRaises(rfmodel.RFModelError):
            self.lib.renormalize_conversion_power_waves(1, [(0, 0)], [[.2]], [50], [50 + 1j])
        for arguments in (dict(external_channels=[0, 0]), dict(external_channels=[1]),
                          dict(external_channels=[], source=[1]),
                          dict(external_channels=[0], source=[complex(math.nan, 0)]),
                          dict(external_channels=[0], source_covariance=[[1]], source_complementary=[[2]])):
            with self.assertRaises((ValueError, rfmodel.RFModelError)):
                self.lib.renormalize_conversion_boundary(1, [(0, 1)], [50], [70 + 8j], **arguments)
        for fields in (dict(output_offset=[math.inf]), dict(intrinsic_covariance=[[-1]]),
                       dict(intrinsic_covariance=[[1]], intrinsic_complementary=[[2]])):
            with self.assertRaises((ValueError, rfmodel.RFModelError)):
                self.lib.renormalize_conversion_power_waves(1, [(0, 1)], [[.2]], [50], [70 + 8j], **fields)

    def test_singular_transforms_and_complex_dc_boundaries_fail_without_mutation(self):
        direct, offset, c, p = [[3]], [.2j], [[1]], [[.3j]]
        saved = copy.deepcopy((direct, offset, c, p))
        with self.assertRaises(rfmodel.RFModelError):
            self.lib.renormalize_conversion_power_waves(
                1, [(0, 1)], direct, [50], [100], output_offset=offset,
                intrinsic_covariance=c, intrinsic_complementary=p)
        self.assertEqual((direct, offset, c, p), saved)
        with self.assertRaises(rfmodel.RFModelError):
            self.lib.renormalize_conversion_boundary(
                1, [(0, 1)], [1], [2], external_channels=[0], reflection=[3], source=[1])
        for scale in (1e-24, 1e-12, 1, 1e12, 1e24):
            with self.assertRaises(rfmodel.RFModelError):
                self.lib.renormalize_conversion_boundary(
                    1, [(0, 1)], [50 * scale], [100 * scale], external_channels=[0],
                    reflection=[3], source=[.4 + .2j], source_covariance=[[2]],
                    source_complementary=[[.3 + .2j]])
            neighbor = self.lib.renormalize_conversion_boundary(
                1, [(0, 1)], [50 * scale], [100 * scale], external_channels=[0],
                reflection=[3 + 1e-8], source=[.4 + .2j])
            self.assertTrue(math.isfinite(abs(neighbor.source[0])))
        for fields in (dict(source=[1j]), dict(reflection=[.1j]),
                       dict(source_covariance=[[1]], source_complementary=[[0]])):
            with self.assertRaises(rfmodel.RFModelError):
                self.lib.renormalize_conversion_boundary(
                    1, [(0, 0)], [50], [73], external_channels=[0], **fields)
        empty = self.lib.renormalize_conversion_boundary(
            1, [(0, 1)], [50], [73 + 8j], external_channels=[])
        self.assertEqual(empty.source, (0,))
        self.assertEqual(empty.reflection, (0,))
        self.assertEqual(empty.source_covariance, ((0,),))

    def test_extreme_references_follow_independent_norton_thevenin_formulas(self):
        resistance, impedance, vs = 75., 45 + 3j, .4 + .2j
        old = 1 + 0j
        scattering = (resistance - old.conjugate()) / (resistance + old)
        offset = -.02 + .01j
        gamma = (impedance - old) / (impedance + old.conjugate())
        source = vs / (impedance + old.conjugate())
        for scale in (1e-24, 1e-12, 1, 1e12, 1e24):
            for new in (complex(scale), scale * (1 + .3j)):
                with self.subTest(reference=new):
                    change = self.lib.renormalize_conversion_power_waves(
                        1, [(0, 1)], [[scattering]], [old], [new], output_offset=[offset],
                        intrinsic_covariance=[[2]], intrinsic_complementary=[[.3 + .2j]])
                    transfer = math.sqrt(new.real) * (resistance + old) / (resistance + new)
                    self.near(change.direct[0][0], (resistance - new.conjugate()) / (resistance + new))
                    self.near(change.noise_direct[0][0], transfer, abs(transfer))
                    self.near(change.noise_conjugate[0][0], 0, abs(transfer))
                    self.near(change.output_offset[0], transfer * offset, abs(transfer * offset))
                    self.near(change.intrinsic_covariance[0][0], 2 * abs(transfer) ** 2, abs(transfer) ** 2)
                    self.near(change.intrinsic_complementary[0][0], (.3 + .2j) * transfer ** 2, abs(transfer) ** 2)
                    boundary = self.lib.renormalize_conversion_boundary(
                        1, [(0, 1)], [old], [new], external_channels=[0],
                        source=[source], reflection=[gamma], source_covariance=[[2]], source_complementary=[[.3j]])
                    transfer = math.sqrt(new.real) * (impedance + old.conjugate()) / (impedance + new.conjugate())
                    self.near(boundary.source[0], vs * math.sqrt(new.real) / (impedance + new.conjugate()),
                              abs(boundary.source[0]))
                    self.near(boundary.reflection[0], (impedance - new) / (impedance + new.conjugate()))
                    self.near(boundary.source_covariance[0][0], 2 * abs(transfer) ** 2, abs(transfer) ** 2)
                    self.near(boundary.source_complementary[0][0], .3j * transfer ** 2, abs(transfer) ** 2)

    def test_ideal_through_needs_no_invertible_physical_z(self):
        result = self.lib.renormalize_conversion_power_waves(
            1, [(0, 1), (1, 1)], [[0, 1], [1, 0]], [50, 50], [31 + 12j, 73 - 21j])
        self.assertTrue(all(math.isfinite(value.real) and math.isfinite(value.imag)
                            for row in result.direct for value in row))
        self.near(result.conjugate, [[0, 0], [0, 0]])
        restored = self.lib.renormalize_conversion_power_waves(
            1, [(0, 1), (1, 1)], result.direct, [31 + 12j, 73 - 21j], [50, 50], result.conjugate)
        self.near(restored.direct, [[0, 1], [1, 0]])

    def test_explicit_vectors_rejected_for_automatic_operating_point(self):
        for refs in ([50], [31 + 7j]):
            device = dict(channels=[(0, 1)], direct=[[0]], reference_impedances_ohms=refs)
            with self.assertRaisesRegex(ValueError, 'common real reference'):
                self.lib.conversion_network(1, [device], operating_point={})
            with self.assertRaisesRegex(ValueError, 'common real reference'):
                self.lib.solve_conversion_operating_point(1, [device])

    def test_noise_reference_experiment_coordinate_invariance(self):
        channels = [(0, 1), (1, 1)]
        scattering = [[.2, .8], [.8, .2]]
        intrinsic = self.lib.passive_noise(scattering, 290)
        device = dict(channels=channels, direct=scattering, intrinsic_covariance=intrinsic)
        options = dict(reference_channels=[0], thermal_channels=[0], output_channel=1)
        baseline = self.lib.conversion_noise_analysis(1, [device], **options)
        changed = self.lib.renormalize_conversion_power_waves(
            1, channels, scattering, [50, 50], [31 + 12j, 73 - 21j], intrinsic_covariance=intrinsic)
        boundary = self.lib.renormalize_conversion_boundary(
            1, channels, [50, 50], [31 + 12j, 73 - 21j], external_channels=[0, 1])
        altered = self.lib.conversion_noise_analysis(1, [dict(
            channels=channels, direct=changed.direct, conjugate=changed.conjugate,
            intrinsic_covariance=changed.intrinsic_covariance,
            intrinsic_complementary=changed.intrinsic_complementary,
            reflection=boundary.reflection, reference_impedances_ohms=[31 + 12j, 73 - 21j])], **options)
        for field in rfmodel.ConversionNoiseAnalysis._fields:
            self.near(getattr(altered, field), getattr(baseline, field), 1e-20 if 'noise_w' in field else 1)
        self.near(baseline.reference_gain, .64)
        self.near(baseline.noise_factor, 1.5)
        self.assertEqual(altered.reference_impedances_ohms, (31 + 12j, 73 - 21j))
        self.assertEqual(len(baseline), 6)


if __name__ == '__main__':
    unittest.main()

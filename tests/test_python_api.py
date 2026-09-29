"""Exercise the Python binding against a real, explicitly supplied shared library."""
import gc
from pathlib import Path
import sys
import unittest
import weakref

LIBRARY_PATH = sys.argv.pop(1)
if "--installed" in sys.argv:
    sys.argv.remove("--installed")
else:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from rfmodel import Library, RFModelError


class PythonApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.library = Library(LIBRARY_PATH)

    def test_complex_cascade_and_waves(self):
        with self.library.network(75.) as network:
            self.assertEqual(network.add([[0, -0.5j], [-0.5j, 0]]), 0)
            self.assertEqual(network.add([[0, 0.2], [0.2, 0]]), 2)
            network.connect(1, 2)
            self.assertEqual(network.port_count, 4)
            self.assertAlmostEqual(network.external_s([0, 3])[1][0], -0.1j)
            network.terminate(0, source=2.)
            network.terminate(3)
            waves = network.solve()
            self.assertAlmostEqual(waves.outgoing[3], -0.2j)
            self.assertLess(waves.relative_residual, 1e-12)

    def test_native_error_preserves_network(self):
        with self.library.network() as network:
            with self.assertRaises(RFModelError) as caught:
                network.add([[0, 1], [1, 0]], reference_ohms=75.)
            self.assertEqual(caught.exception.status, 1)
            self.assertIn("reference", str(caught.exception))
            self.assertEqual(network.port_count, 0)
            network.add([[0, 1], [1, 0]])
            network.connect(0, 1)
            with self.assertRaises(RFModelError) as caught:
                network.solve()
            self.assertEqual(caught.exception.status, 2)

    def test_shape_indices_and_nonfinite(self):
        with self.library.network() as network:
            for matrix in ([], [[0, 1]], [[0], [1]]):
                with self.assertRaises(ValueError):
                    network.add(matrix)
            with self.assertRaises(RFModelError):
                network.add([[float("nan")]])
            network.add([[0, 1], [1, 0]])
            for index in (-1, 2**80, 0.5, True):
                with self.assertRaises((ValueError, TypeError)):
                    network.terminate(index)
            with self.assertRaises(RFModelError):
                network.external_s([0, 0])
            self.assertEqual(network.external_s([1, 0]), ((0j, 1+0j), (1+0j, 0j)))

    def test_context_exception_and_double_close(self):
        network = self.library.network()
        with self.assertRaisesRegex(ValueError, "user exception"):
            with network:
                raise ValueError("user exception")
        network.close()
        for action in (lambda: network.port_count, network.solve,
                       lambda: network.external_s([0]), lambda: network.add([[0]])):
            with self.assertRaisesRegex(RuntimeError, "closed"):
                action()

    def test_finalizer_does_not_retain_network(self):
        network = self.library.network()
        reference = weakref.ref(network)
        finalizer = network._finalizer
        del network
        gc.collect()
        self.assertIsNone(reference())
        self.assertFalse(finalizer.alive)

    def test_invalid_creation(self):
        with self.assertRaises(RFModelError):
            self.library.network(-1.)
        with self.assertRaises(FileNotFoundError):
            Library(Path(LIBRARY_PATH).parent / "missing-rfmodel-library")


if __name__ == "__main__":
    unittest.main()

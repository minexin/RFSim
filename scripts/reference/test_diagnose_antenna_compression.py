import importlib.util
import math
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "diagnostic", Path(__file__).with_name("diagnose-antenna-compression.py"))
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)


class CompressionTests(unittest.TestCase):
    def test_p1db_output_and_small_signal_limit(self):
        gain, output = 100., .01
        input_power = output / (gain * 10 ** (-.1))
        actual = diagnostic.cubic_power(input_power, gain, output)
        self.assertAlmostEqual(actual / output, 1., places=12)
        self.assertAlmostEqual(10 * math.log10(actual / input_power), 19., places=12)
        self.assertAlmostEqual(diagnostic.cubic_power(1e-20, gain, output) / 1e-20, gain)

    def test_out_of_domain(self):
        for power in (0., -1., float("nan"), 1.):
            with self.assertRaises(ValueError):
                diagnostic.cubic_power(power, 100., .01)


if __name__ == "__main__":
    unittest.main()

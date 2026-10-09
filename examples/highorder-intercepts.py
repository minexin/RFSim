"""Build an explicit intercept-defined polynomial and use an existing system graph."""

import argparse
import json

from rfmodel import InterceptReference, Library, TwoToneIntercept
from rfmodel.coherent_system_file import analyze_coherent_system

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--library", required=True)
args = parser.parse_args()
library = Library(args.library)
coefficients = library.polynomial_coefficients_from_intercepts(
    10.0,
    [
        TwoToneIntercept(1, 1, 40.0, 1, InterceptReference.OUTPUT),
        TwoToneIntercept(2, -1, 30.0, -1, InterceptReference.OUTPUT),
        TwoToneIntercept(3, -1, 33.0, 1, InterceptReference.OUTPUT),
    ],
)
# This example defines a polynomial, not an RFAMP compression/limiting model.
model = {
    "format": "rfmodel.coherent-system",
    "version": 1,
    "spacing_hz": 1e8,
    "sources": [{"id": "first"}, {"id": "second"}],
    "inputs": [
        {
            "id": "input",
            "components": [
                {"source": "first", "bin": 10, "bandwidth_hz": 1, "amplitude": [0.001, 0.0]},
                {"source": "second", "bin": 11, "bandwidth_hz": 1, "amplitude": [0.0, 0.001]},
            ],
        }
    ],
    "stages": [
        {
            "id": "amplifier",
            "type": "polynomial_amplifier",
            "input": "input",
            "output": "result",
            "voltage_coefficients": list(coefficients),
            "max_source_order": 4,
        }
    ],
    "outputs": ["result"],
}
result = analyze_coherent_system(library, model)
print(json.dumps({"model": model, "result": result}, indent=2, allow_nan=False))

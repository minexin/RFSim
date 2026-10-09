"""Two RF-only polynomial stages; retain local origin terms for the caller."""

import argparse
import json

from rfmodel import CoherentComponent, Library, SpectrumKind

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--library", required=True)
args = parser.parse_args()
library = Library(args.library)
source = CoherentComponent(10, SpectrumKind.SOURCE, 1.0, 7, complex(0.01, 0.003))
first = library.coherent_polynomial(1e8, [source], [0.0, 1.0, 0.5])
second = library.coherent_polynomial(
    1e8, [term.component for term in first.terms], [0.0, 0.8, 0.4, -0.1]
)


def encode(response):
    return {
        "inputs": [
            {
                "bin": item.bin,
                "kind": item.kind.name.lower(),
                "group": item.coherence_group,
                "bandwidth_hz": item.bandwidth_hz,
                "wave": [item.amplitude.real, item.amplitude.imag],
            }
            for item in response.inputs
        ],
        "terms": [
            {
                "order": item.order,
                "input_indices": item.input_indices,
                "bin": item.component.bin,
                "group": item.component.coherence_group,
                "wave": [item.component.amplitude.real, item.component.amplitude.imag],
            }
            for item in response.terms
        ],
    }


print(json.dumps({"first": encode(first), "second": encode(second)}, indent=2))

"""Run with PYTHONPATH=python and one argument: the RFModel shared library."""
import json
import math
import sys

from rfmodel import (
    CoherentComponent, CoherentMixerInput, Library, PortCoherentComponent,
    SourceCoherence, SpectrumKind,
)


def analyze(library, independent_lo):
    rf, lo_a, lo_b = library.assign_source_coherence([
        SourceCoherence("rf"),
        SourceCoherence("lo-a", "lo-clock"),
        SourceCoherence("lo-b", "" if independent_lo else "lo-clock"),
    ])
    first = CoherentComponent(10, SpectrumKind.SOURCE, 1., rf, 1.)
    second = first._replace(amplitude=1j)
    waves = library.mix_coherent_components(1e8, [
        CoherentMixerInput(first, 8, 0., 0., lo_a),
        CoherentMixerInput(second, 8, 0., math.pi / 2, lo_b),
    ])
    with library.network() as network:
        k = 1 / math.sqrt(2)
        network.add([[0, 0, k], [0, 0, k], [k, k, 0]])
        result = network.transmit_coherent(
            spacing_hz=1e8,
            components=[PortCoherentComponent(i // 2, c) for i, c in enumerate(waves)],
            external_ports=[2, 0, 1], output_port=2)
    return {"difference_200MHz_w": result.power_by_bin_w[2],
            "sum_1800MHz_w": result.power_by_bin_w[18]}


def main():
    library = Library(sys.argv[1])
    print(json.dumps({
        "locked_lo": analyze(library, False),
        "independent_lo": analyze(library, True),
    }, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()

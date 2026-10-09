"""python -m rfmodel: execute RF network, spectrum, or explicit coherence models."""
import argparse
import json
from pathlib import Path
import sys
from . import Library
from .model_file import analyze, load, referenced_touchstone_paths
from .spectrum_file import analyze_spectrum
from .amplifier_file import analyze_amplifier
from .coherence_file import analyze_coherence
from .coherent_network_file import analyze_coherent_network
from .coherent_system_file import analyze_coherent_system


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.output.resolve() in (args.model.resolve(), args.library.resolve()):
            raise ValueError("Output must not overwrite the model or native library")
        document = load(args.model)
        if not isinstance(document, dict):
            raise ValueError("Model must be an object")
        if args.output.resolve() in referenced_touchstone_paths(document, args.model.parent):
            raise ValueError("Output must not overwrite Touchstone input data")
        operation = {"rfmodel.coherence": analyze_coherence,
                     "rfmodel.coherent-network": analyze_coherent_network,
                     "rfmodel.coherent-system": analyze_coherent_system,
                     "rfmodel.spectrum-chain": analyze_spectrum,
                     "rfmodel.amplifier-components": analyze_amplifier}.get(document.get("format"), analyze)
        result = operation(Library(args.library), document, base_directory=args.model.parent)
        text = json.dumps(result, indent=2, allow_nan=False) + "\n"
        args.output.write_text(text, encoding="utf-8")
        return 0
    except (OSError, ValueError, TypeError, OverflowError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

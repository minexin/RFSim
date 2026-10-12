"""python -m rfmodel: execute RF analyses and declarative optimization/Monte Carlo tasks."""

import argparse
import json
from pathlib import Path
import sys
from . import Library
from .model_file import load, referenced_touchstone_paths
from .analysis_file import analyze_document
from .optimization_file import analyze_optimization
from .monte_carlo_file import analyze_monte_carlo


def _same_file(first, second):
    return (first.resolve() == second.resolve() or
            (first.exists() and second.exists() and first.samefile(second)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        if any(_same_file(args.output, source) for source in (args.model, args.library)):
            raise ValueError("Output must not overwrite the model or native library")
        document = load(args.model)
        if not isinstance(document, dict):
            raise ValueError("Model must be an object")
        if any(_same_file(args.output, source)
               for source in referenced_touchstone_paths(document, args.model.parent)):
            raise ValueError("Output must not overwrite Touchstone input data")
        operation = {'rfmodel.optimization': analyze_optimization,
                     'rfmodel.monte-carlo': analyze_monte_carlo}.get(document.get('format'), analyze_document)
        result = operation(Library(args.library), document, base_directory=args.model.parent)
        text = json.dumps(result, indent=2, allow_nan=False) + "\n"
        args.output.write_text(text, encoding="utf-8")
        return 0
    except (OSError, ValueError, TypeError, OverflowError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

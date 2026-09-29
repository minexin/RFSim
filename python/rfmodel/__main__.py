"""python -m rfmodel: execute a linear-network model file."""
import argparse
import json
from pathlib import Path
import sys
from . import Library
from .model_file import analyze, load


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.output.resolve() in (args.model.resolve(), args.library.resolve()):
            raise ValueError("Output must not overwrite the model or native library")
        result = analyze(Library(args.library), load(args.model))
        text = json.dumps(result, indent=2, allow_nan=False) + "\n"
        args.output.write_text(text, encoding="utf-8")
        return 0
    except (OSError, ValueError, TypeError, OverflowError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

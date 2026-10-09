"""Preserve cancelling provenance and distribute products through the C ABI."""

import sys

from rfmodel import Library, OriginContribution


def main():
    library = Library(sys.argv[1])
    contributions = (
        OriginContribution(((7, 1),), 1.0),
        OriginContribution(((9, 1),), -1.0),
    )
    square = library.product_origin_expressions([contributions], [1, 1])
    print(f"Total wave: {square.amplitude}")
    for term in square.terms:
        print(f"{term.factors}: {term.amplitude}")
    assert square.amplitude == 0
    assert tuple(term.amplitude for term in square.terms) == (1, -2, 1)


if __name__ == "__main__":
    main()

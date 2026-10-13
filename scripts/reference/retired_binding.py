"""Explicit failure for historical binding-backed numerical replay.

Capture parsing/scoring remains importable without the retired product package.
The native high-order replay protocol has its own C++ implementation. Other
historical numerical adapters can be reproduced at the pinned baseline below.
"""
BASELINE = "e47ea901a2ded2528f7e26502d8635f4d599ab24"


def unavailable(*args, **kwargs):
    raise RuntimeError(
        "This historical numerical replay used the retired Python product interface. "
        "Use the C++ simulation API for new integration, --native-probe for supported "
        "high-order replays, or the archived source at commit " + BASELINE + ". "
        "No reference comparison was performed. See docs/python-interface-retirement.md."
    )


Library = unavailable

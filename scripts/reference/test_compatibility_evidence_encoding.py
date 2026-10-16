"""Exercise the complete evidence validator under a non-UTF-8 text default."""

import contextlib
import io
from pathlib import Path
import runpy
import sys
import unittest
from unittest import mock


class EvidenceEncodingTests(unittest.TestCase):
    def test_full_registry_and_mutations_with_gbk_default(self):
        script = Path(__file__).with_name("check_compatibility_evidence.py")
        original_open = Path.open

        def legacy_default(path, mode="r", buffering=-1, encoding=None,
                           errors=None, newline=None):
            # Model the Chinese Windows default that exposed the real failure.
            # Explicit codecs and all binary hash reads retain their semantics.
            if "b" not in mode and encoding in (None, "locale"):
                encoding = "gbk"
            return original_open(path, mode, buffering, encoding, errors, newline)

        output = io.StringIO()
        with mock.patch.object(Path, "open", legacy_default), \
                mock.patch.object(sys, "argv", [str(script), "--self-test"]), \
                contextlib.redirect_stdout(output):
            runpy.run_path(str(script), run_name="__main__")
        self.assertIn("raw-CI binding mutations rejected", output.getvalue())
        self.assertIn("Compatibility evidence metadata valid", output.getvalue())


if __name__ == "__main__":
    unittest.main()

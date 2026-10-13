"""Check supported capture/scoring imports with product bindings actively blocked."""
import ast
import importlib.abc
import importlib.util
from pathlib import Path
import sys
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

class RejectProduct(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname == "rfmodel" or fullname.startswith("rfmodel."):
            raise AssertionError("Retired product import attempted: " + fullname)

sys.meta_path.insert(0, RejectProduct())

class RetirementTests(unittest.TestCase):
    def test_no_product_package(self):
        self.assertFalse((ROOT / "python").exists())
        self.assertFalse((ROOT / "python/pyproject.toml").exists())

    def test_capture_and_scoring_imports(self):
        for path in sorted(HERE.glob("*.py")):
            if path.name.startswith("test_") or path.name == "verify-rf-parameters.py":
                # Independent DOM audit requires lxml; isolated stdlib import test excludes it.
                continue
            with self.subTest(path=path.name):
                name = "retirement_" + path.stem.replace("-", "_")
                spec = importlib.util.spec_from_file_location(name, path)
                module = importlib.util.module_from_spec(spec)
                sys.modules[name] = module
                spec.loader.exec_module(module)

    def test_no_hidden_binding_import(self):
        for path in HERE.glob("*.py"):
            for item in ast.walk(ast.parse(path.read_text(encoding="utf-8-sig"))):
                if isinstance(item, ast.ImportFrom):
                    self.assertFalse((item.module or "").split(".")[0] == "rfmodel", str(path))
                elif isinstance(item, ast.Import):
                    self.assertFalse(any(a.name.split(".")[0] == "rfmodel" for a in item.names), str(path))

    def test_historical_replay_fails_explicitly(self):
        from retired_binding import Library
        with self.assertRaisesRegex(RuntimeError, "No reference comparison was performed"):
            Library("arbitrary-library-path")

if __name__ == "__main__":
    unittest.main()

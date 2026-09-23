"""Regression tests for inventory provenance and model-table selection."""
import hashlib
import importlib.util
import sqlite3
import tempfile
import unittest
import zlib
from contextlib import closing
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "extract_rf_models", Path(__file__).with_name("extract-rf-models.py"))
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


class InventoryTests(unittest.TestCase):
    def test_select_model_column_only(self):
        parser = inventory.ModelTables()
        parser.feed('''<a href="navigation.html">Navigation</a>
            <table><tr><th>Model</th><th>Description</th></tr>
            <tr><td><a href="A.html"><b>A &amp; B</b></a></td>
            <td><a href="background.html">Background</a></td></tr></table>
            <table><tr><th>Category</th></tr>
            <tr><td><a href="other.html">Other</a></td></tr></table>''')
        self.assertEqual(parser.tables, [[("A.html", "A & B")]])

    def test_inventory_and_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "help.qch"
            catalog_page = b"catalog"
            part_page = b'''<table><tr><th>Model</th></tr>
                <tr><td><a href="../models/A%20B.html#one">AB</a></td></tr>
                <tr><td><a href="../models/A%20B.html#two">Alias</a></td></tr>
                </table>'''
            with closing(sqlite3.connect(path)) as database, database:
                database.execute("CREATE TABLE FileNameTable (FileId INTEGER, Name TEXT)")
                database.execute("CREATE TABLE FileDataTable (Id INTEGER, Data BLOB)")
                for number, (name, content) in enumerate([
                    ("catalog.html", catalog_page), ("parts/one.html", part_page),
                    ("parts/two.html", b"no model table"), ("models/A B.html", b"model")
                ]):
                    database.execute("INSERT INTO FileNameTable VALUES (?,?)", (number, name))
                    database.execute("INSERT INTO FileDataTable VALUES (?,?)", (
                        number, len(content).to_bytes(4, "big") + zlib.compress(content)))
            catalog = {
                "source_page": "catalog.html",
                "source_page_sha256": hashlib.sha256(catalog_page).hexdigest(),
                "parts": [{"help_page": "parts/one.html"}, {"help_page": "parts/two.html"}],
            }
            result = inventory.extract(path, catalog)
            self.assertEqual(result["unique_model_count"], 1)
            self.assertEqual(result["part_model_link_count"], 1)
            self.assertEqual(result["models"][0]["names"], ["AB", "Alias"])
            self.assertEqual(result["parts_needing_manual_review"], ["parts/two.html"])
            self.assertEqual(result, inventory.extract(path, catalog))
            with closing(sqlite3.connect(path)) as database, database:
                database.execute("DELETE FROM FileNameTable WHERE Name='models/A B.html'")
            with self.assertRaisesRegex(ValueError, "Missing or ambiguous"):
                inventory.extract(path, catalog)
            catalog["source_page_sha256"] = "incorrect"
            with self.assertRaisesRegex(ValueError, "different help content"):
                inventory.extract(path, catalog)


if __name__ == "__main__":
    unittest.main()

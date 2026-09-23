"""Inventory model links from RF Design part tables in installed Qt help."""
import argparse
import hashlib
import json
import posixpath
import sqlite3
import zlib
from contextlib import closing
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit


class ModelTables(HTMLParser):
    def __init__(self):
        super().__init__()
        self.tables = []
        self.current = None
        self.header = None
        self.anchor = None
        self.depth = 0
        self.column = 0

    def handle_starttag(self, tag, attributes):
        if tag == "table":
            if self.depth == 0:
                self.current = {"headers": [], "links": []}
            self.depth += 1
        if self.depth != 1:
            return
        if tag == "tr":
            self.column = 0
        if tag in ("th", "td"):
            self.column += 1
        if tag == "th":
            self.header = []
        if tag == "a" and self.column == 1:
            self.anchor = [dict(attributes).get("href"), []]

    def handle_data(self, data):
        if self.depth != 1:
            return
        if self.header is not None:
            self.header.append(data)
        if self.anchor is not None:
            self.anchor[1].append(data)

    def handle_endtag(self, tag):
        if self.depth == 1:
            if tag == "th" and self.header is not None:
                self.current["headers"].append(" ".join("".join(self.header).split()))
                self.header = None
            if tag == "a" and self.anchor is not None:
                href, text = self.anchor
                if href:
                    self.current["links"].append((href, " ".join("".join(text).split())))
                self.anchor = None
            if tag == "table":
                if self.current["headers"] in (["Model"], ["Model", "Description"]):
                    self.tables.append(self.current["links"])
                self.current = None
                self.header = None
                self.anchor = None
        if tag == "table":
            self.depth = max(0, self.depth - 1)


def read_page(database, page):
    rows = database.execute(
        "SELECT d.Data FROM FileNameTable f JOIN FileDataTable d "
        "ON f.FileId=d.Id WHERE f.Name=?", (page,)
    ).fetchall()
    if len(rows) != 1:
        raise ValueError("Missing or ambiguous help page: " + page)
    compressed = rows[0][0]
    content = zlib.decompress(compressed[4:])
    if len(content) != int.from_bytes(compressed[:4], "big"):
        raise ValueError("Qt page length mismatch: " + page)
    return content


def extract(qch_path, catalog):
    parts = []
    models = {}
    with closing(sqlite3.connect(qch_path.resolve().as_uri() + "?mode=ro", uri=True)) as database:
        source = read_page(database, catalog["source_page"])
        if hashlib.sha256(source).hexdigest() != catalog["source_page_sha256"]:
            raise ValueError("Catalog was generated from different help content")
        for part in catalog["parts"]:
            page = part["help_page"]
            content = read_page(database, page)
            parser = ModelTables()
            parser.feed(content.decode("utf-8"))
            targets = set()
            for table in parser.tables:
                for href, name in table:
                    link = urlsplit(href)
                    if link.scheme or link.netloc or not link.path or not name:
                        raise ValueError("Invalid model link in " + page + ": " + href)
                    target = posixpath.normpath(posixpath.join(
                        posixpath.dirname(page), unquote(link.path)))
                    if target not in models:
                        model_content = read_page(database, target)
                        models[target] = {
                            "help_page": target,
                            "source_page_sha256": hashlib.sha256(model_content).hexdigest(),
                            "names": set(), "part_pages": set(),
                            "assessment": "unassessed",
                            "systemvue_comparison": "not_executed",
                        }
                    models[target]["names"].add(name)
                    models[target]["part_pages"].add(page)
                    targets.add(target)
            parts.append({
                "help_page": page,
                "source_page_sha256": hashlib.sha256(content).hexdigest(),
                "model_pages": sorted(targets),
                "extraction_status": "model_table_found" if targets else "needs_manual_review",
            })
    for model in models.values():
        model["names"] = sorted(model["names"])
        model["part_pages"] = sorted(model["part_pages"])
    return {
        "schema_version": 1,
        "baseline": "SystemVue 2023 installed help",
        "source_qch": str(qch_path.resolve()),
        "catalog_source_sha256": catalog["source_page_sha256"],
        "coverage_meaning": "Documentation links only; no model is declared compatible.",
        "part_count": len(parts), "unique_model_count": len(models),
        "part_model_link_count": sum(len(part["model_pages"]) for part in parts),
        "parts_needing_manual_review": [part["help_page"] for part in parts
                                        if part["extraction_status"] == "needs_manual_review"],
        "parts": sorted(parts, key=lambda part: part["help_page"]),
        "models": [models[page] for page in sorted(models)],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("qch", type=Path)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = extract(args.qch, json.loads(args.catalog.read_text(encoding="utf-8")))
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in (
        "part_count", "unique_model_count", "part_model_link_count", "parts_needing_manual_review")}))


if __name__ == "__main__":
    main()

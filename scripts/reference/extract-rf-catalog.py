"""Extract factual part names/links from the installed SystemVue help index."""
import argparse
import hashlib
import json
import posixpath
import sqlite3
import zlib
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit


class PartLinks(HTMLParser):
    def __init__(self):
        super().__init__()
        self.href = None
        self.text = []
        self.links = []

    def handle_starttag(self, tag, attributes):
        if tag == "a":
            self.href = dict(attributes).get("href")
            self.text = []

    def handle_data(self, data):
        if self.href is not None:
            self.text.append(data)

    def handle_endtag(self, tag):
        if tag == "a" and self.href is not None:
            self.links.append((self.href, " ".join("".join(self.text).split())))
            self.href = None


def extract(qch_path):
    page = "rfdesign/RF_Design_Kit_Library.html"
    with sqlite3.connect(qch_path.resolve().as_uri() + "?mode=ro", uri=True) as database:
        rows = database.execute(
            "SELECT d.Data FROM FileNameTable f JOIN FileDataTable d "
            "ON f.FileId=d.Id WHERE f.Name=?", (page,)
        ).fetchall()
        if len(rows) != 1:
            raise ValueError("RF Design catalog page is missing or ambiguous")
        compressed = rows[0][0]
        content = zlib.decompress(compressed[4:])
        if len(content) != int.from_bytes(compressed[:4], "big"):
            raise ValueError("Qt help page length mismatch")
        known_pages = {row[0] for row in database.execute("SELECT Name FROM FileNameTable")}
    parser = PartLinks()
    parser.feed(content.decode("utf-8"))
    parts = {}
    for href, title in parser.links:
        path = unquote(urlsplit(href).path)
        if not path.endswith("_Part.html"):
            continue
        target = posixpath.normpath(posixpath.join(posixpath.dirname(page), path))
        if target not in known_pages or not title:
            raise ValueError("Unresolved catalog entry: " + href)
        parts.setdefault(target, set()).add(title)
    if not parts:
        raise ValueError("No part links found")
    return {
        "schema_version": 1,
        "baseline": "SystemVue 2023 installed help",
        "source_qch": str(qch_path.resolve()),
        "source_page": page,
        "source_page_sha256": hashlib.sha256(content).hexdigest(),
        "unique_part_count": len(parts),
        "coverage_meaning": "Catalog inventory only; no part is declared compatible.",
        "parts": [
            {"help_page": target, "catalog_names": sorted(names),
             "assessment": "unassessed", "systemvue_comparison": "not_executed"}
            for target, names in sorted(parts.items())
        ],
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("qch", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = extract(args.qch)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"parts": result["unique_part_count"], "output": str(args.output)}))


if __name__ == "__main__":
    main()

"""Offline parameter provenance, parsing, CLI safety and committed inventory tests."""
import copy
import importlib.util
import json
import os
import sqlite3
import subprocess
import sys
import tempfile
import unittest
import zlib
from contextlib import closing
from pathlib import Path
from unittest.mock import patch

SCRIPT = Path(__file__).with_name('extract-rf-parameters.py').resolve()
spec = importlib.util.spec_from_file_location('parameters', SCRIPT)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
HEADER = '<tr><th>Name</th><th>Description</th><th>Default</th><th>Units</th><th>Type</th><th>Runtime Tunable</th></tr>'
ROW = '<tr><td>G</td><td>private description</td><td>20</td><td>dB10</td><td>Float</td><td>NO</td></tr>'


def page(rows=ROW):
    return ('<h2>Model Parameters</h2><table>' + HEADER + rows + '</table>').encode()


class ParseTests(unittest.TestCase):
    def test_exact_text_empty_and_absent_columns(self):
        r = m.parse_page(page('<tr><td>A&amp;B</td><td>private description</td><td><b>[1;</b><br>2]</td><td></td><td>None</td><td>NO</td></tr>'))
        p = r['parameters'][0]
        self.assertEqual((p['name'], p['default_text'], p['unit_text'], p['range_text']), ('A&B', '[1; 2]', '', None))
        self.assertEqual(p['type_text'], 'None')
        self.assertEqual((p['table_index'], p['row_index']), (1, 2))
        self.assertNotIn('private description', json.dumps(r))
        self.assertEqual(r['manual_review'], [])
        self.assertEqual(m.text_digest(b'{\r\n}\r\n'), m.text_digest(b'{\n}\n'))

    def test_range_not_evaluated(self):
        s = page().decode().replace('</th></tr>', '</th><th>Range</th></tr>', 1).replace('</td></tr>', '</td><td>[0:&#8734;)</td></tr>', 1)
        r = m.parse_page(s.encode())
        self.assertEqual(r['parameters'][0]['range_text'], '[0:\u221e)')

    def test_port_tables_keep_section_and_no_description(self):
        s = '<h2>Input Ports</h2><table><tr><th>Port</th><th>Name</th><th>Description</th><th>Signal Type</th><th>Optional</th></tr><tr><td>1..N</td><td>in</td><td>private</td><td>anytype</td><td>YES</td></tr></table>'
        r = m.parse_page(page() + s.encode())
        self.assertEqual(r['port_tables'][0]['section'], 'Input Ports')
        self.assertEqual(r['port_tables'][0]['port_text'], '1..N')
        self.assertNotIn('private', json.dumps(r))

    def test_duplicate_kept_with_review(self):
        r = m.parse_page(page() + page())
        self.assertEqual(len(r['parameters']), 2)
        self.assertIn('duplicate_parameter_names', r['manual_review'])
        self.assertEqual([p['table_index'] for p in r['parameters']], [1, 2])

    def test_unknown_table_and_no_records(self):
        r = m.parse_page(b'<table><tr><th>Parameter</th><th>Description</th></tr><tr><td>X</td><td>private</td></tr></table>')
        self.assertEqual(r['parameters'], [])
        self.assertIn('table_1:unrecognized_metadata_header', r['manual_review'])
        self.assertIn('no_parameter_records', r['manual_review'])

    def test_nested_spanning_images_and_short_rows_fail_closed(self):
        for row in (ROW.replace('20', '<table><tr><td>20</td></tr></table>'),
                    ROW.replace('<td>20', '<td colspan="2">20'),
                    ROW.replace('20', '<img src="formula.png">'),
                    ROW.replace('<td>NO</td>', '')):
            with self.subTest(row=row):
                r = m.parse_page(page(row))
                self.assertEqual(r['parameters'], [])
                self.assertTrue(r['manual_review'])

    def test_replacement_character_flagged_and_retained(self):
        r = m.parse_page(page(ROW.replace('20', '\ufffd')))
        self.assertEqual(r['parameters'][0]['default_text'], '\ufffd')
        self.assertIn('table_1:replacement_character_in_source', r['manual_review'])

    def test_unclosed_and_invalid_encoding(self):
        r = m.parse_page(page()[:-8])
        self.assertIn('unclosed_table', r['manual_review'])
        with self.assertRaises(UnicodeDecodeError):
            m.parse_page(b'\xff')


class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.qch = self.root / 'help.qch'
        self.pages = {
            'rfdesign/RF_Design_Kit_Library.html': b'<a href="Test_Part.html">Test</a>',
            'rfdesign/Test_Part.html': b'<table><tr><th>Model</th></tr><tr><td><a href="A.html">A</a></td></tr><tr><td><a href="../algorithm/B.html">B</a></td></tr></table>',
            'rfdesign/A.html': page(), 'algorithm/B.html': b'<h1>B</h1>no parameter table',
        }
        with closing(sqlite3.connect(self.qch)) as db, db:
            db.execute('CREATE TABLE FileNameTable (FileId INTEGER, Name TEXT)')
            db.execute('CREATE TABLE FileDataTable (Id INTEGER, Data BLOB)')
            for i, (name, content) in enumerate(self.pages.items()):
                db.execute('INSERT INTO FileNameTable VALUES (?,?)', (i, name))
                db.execute('INSERT INTO FileDataTable VALUES (?,?)', (i, len(content).to_bytes(4, 'big') + zlib.compress(content)))
        self.catalog = m.catalog_tool.extract(self.qch)
        self.models = m.models_tool.extract(self.qch, self.catalog)
        self.cat_path, self.models_path = self.root / 'catalog.json', self.root / 'models.json'
        self.cat_path.write_text(json.dumps(self.catalog), encoding='utf-8')
        self.models_path.write_text(json.dumps(self.models), encoding='utf-8')

    def run_cli(self, output):
        return subprocess.run([sys.executable, str(SCRIPT), str(self.qch), str(self.cat_path),
                               str(self.models_path), str(output)], capture_output=True, text=True)

    def test_inventory_deterministic_cross_library_and_releases_database(self):
        r = m.extract(self.qch, self.catalog, self.models)
        self.assertEqual((r['part_count'], r['model_count'], r['part_model_link_count'], r['parameter_count']), (1, 2, 2, 1))
        self.assertEqual(r['models_needing_manual_review'], ['algorithm/B.html'])
        self.assertEqual(r, m.extract(self.qch, self.catalog, self.models))
        moved = self.qch.with_suffix('.renamed')
        self.qch.rename(moved)
        moved.rename(self.qch)
        self.assertEqual(r['source_qch_sha256'], m.file_digest(self.qch))

    def test_inventory_tampering_rejected(self):
        for field, value in [('part_count', 0), ('part_model_link_count', 99),
                             ('models', self.models['models'][:1])]:
            invalid = copy.deepcopy(self.models)
            invalid[field] = value
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, 'inventory'):
                m.extract(self.qch, self.catalog, invalid)
        invalid = copy.deepcopy(self.catalog)
        invalid['parts'][0]['assessment'] = 'supported'
        with self.assertRaisesRegex(ValueError, 'Catalog'):
            m.extract(self.qch, invalid, self.models)

    def test_changed_missing_ambiguous_and_bad_qt_pages_rejected(self):
        for replacement in [b'new content', None, 'duplicate', 'bad_length']:
            with self.subTest(replacement=replacement):
                backup = self.qch.read_bytes()
                with closing(sqlite3.connect(self.qch)) as db, db:
                    if replacement is None:
                        db.execute("DELETE FROM FileNameTable WHERE Name='rfdesign/A.html'")
                    elif replacement == 'duplicate':
                        db.execute("INSERT INTO FileNameTable SELECT * FROM FileNameTable WHERE Name='rfdesign/A.html'")
                    else:
                        data = (999).to_bytes(4, 'big') + zlib.compress(b'old') if replacement == 'bad_length' else len(replacement).to_bytes(4, 'big') + zlib.compress(replacement)
                        db.execute('UPDATE FileDataTable SET Data=? WHERE Id=2', (data,))
                with self.assertRaises(ValueError):
                    m.extract(self.qch, self.catalog, self.models)
                self.qch.write_bytes(backup)

    def test_database_change_during_extraction_rejected(self):
        with patch.object(m, 'file_digest', side_effect=['first', 'second']):
            with self.assertRaisesRegex(ValueError, 'changed during'):
                m.extract(self.qch, self.catalog, self.models)

    def test_cli_atomic_replay_and_failed_source_preserves_output(self):
        output = self.root / 'result.json'
        self.assertEqual(self.run_cli(output).returncode, 0)
        original = output.read_bytes()
        self.assertEqual(self.run_cli(output).returncode, 0)
        self.assertEqual(output.read_bytes(), original)
        self.models['models'][0]['source_page_sha256'] = 'bad'
        self.models_path.write_text(json.dumps(self.models), encoding='utf-8')
        self.assertNotEqual(self.run_cli(output).returncode, 0)
        self.assertEqual(output.read_bytes(), original)
        self.assertEqual(json.loads(original)['input_files']['catalog_sha256'], m.text_digest(self.cat_path.read_bytes()))

    def test_cli_rejects_input_and_hardlink_outputs(self):
        for source in (self.qch, self.cat_path, self.models_path):
            original = source.read_bytes()
            self.assertNotEqual(self.run_cli(source).returncode, 0)
            alias = self.root / 'alias'
            os.link(source, alias)
            self.assertNotEqual(self.run_cli(alias).returncode, 0)
            self.assertEqual(source.read_bytes(), original)
            alias.unlink()


class CommittedEvidenceTests(unittest.TestCase):
    def test_metadata_matches_catalog_models_and_known_facts(self):
        root = SCRIPT.parents[2]
        d = json.loads((root / 'validation/systemvue-2023-rf-parameters.json').read_text(encoding='utf-8'))
        inventory = json.loads((root / 'validation/systemvue-2023-rf-models.json').read_text(encoding='utf-8'))
        self.assertEqual((d['part_count'], d['model_count'], d['part_model_link_count']), (243, 281, 401))
        self.assertEqual(d['parameter_count'], sum(len(r['parameters']) for r in d['models']))
        self.assertEqual(d['parameter_count'], 3404)
        self.assertEqual(d['models_needing_manual_review'], ['rfdesign/ArrayXParam.html', 'rfdesign/GND.html', 'rfdesign/MultiSource.html'])
        self.assertEqual(d['explicit_port_record_count'], 13)
        by_page = {r['help_page']: r for r in d['models']}
        self.assertEqual(len(by_page), len(inventory['models']))
        for model in inventory['models']:
            r = by_page[model['help_page']]
            for key in ('source_page_sha256', 'part_pages', 'names'):
                self.assertEqual(r[key], model[key])
            self.assertEqual(r['compatibility_status'], 'not_assessed_here')
        for field, filename in [('catalog_sha256', 'systemvue-2023-rf-catalog.json'), ('models_sha256', 'systemvue-2023-rf-models.json')]:
            self.assertEqual(d['input_files'][field], m.text_digest((root / 'validation' / filename).read_bytes()))
        res = {p['name']: p for p in by_page['rfdesign/RES.html']['parameters']}
        self.assertEqual((res['R']['default_text'], res['R']['unit_text'], res['Ta']['default_text']), ('50', 'ohm', ''))
        amp = {p['name']: p for p in by_page['rfdesign/RFAMP.html']['parameters']}
        self.assertEqual((amp['G']['default_text'], amp['G']['unit_text']), ('20', 'dB10'))
        filt = {p['name']: p for p in by_page['rfdesign/LPF_BUTTER.html']['parameters']}
        self.assertEqual((filt['IL']['default_text'], filt['Amax']['default_text']), ('0.01', '100'))


if __name__ == '__main__':
    unittest.main()

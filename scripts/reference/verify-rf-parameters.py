"""Independently check factual parameter rows using lxml, not the extractor parser.

Requires lxml. Reads local vendor help; emits only counts and fingerprints.
"""
import argparse
import hashlib
import json
import sqlite3
import zlib
from contextlib import closing
from pathlib import Path
from lxml import html


def verify(qch, metadata):
    records = json.loads(metadata.read_text(encoding='utf-8'))
    checked = 0
    port_count = 0
    with closing(sqlite3.connect(qch.resolve().as_uri() + '?mode=ro', uri=True)) as db:
        for model in records['models']:
            rows = db.execute('SELECT d.Data FROM FileNameTable f JOIN FileDataTable d ON f.FileId=d.Id WHERE f.Name=?', (model['help_page'],)).fetchall()
            if len(rows) != 1:
                raise ValueError('Missing or ambiguous model page')
            blob = rows[0][0]
            raw = zlib.decompress(blob[4:])
            if len(raw) != int.from_bytes(blob[:4], 'big') or hashlib.sha256(raw).hexdigest() != model['source_page_sha256']:
                raise ValueError('Model source fingerprint mismatch')
            doc = html.fromstring(raw.decode('utf-8'))
            tables = doc.xpath('//table[not(ancestor::table)]')
            extracted = []
            ports = []
            for index, table in enumerate(tables, 1):
                table_rows = table.xpath('./tr|./thead/tr|./tbody/tr|./tfoot/tr')
                values = [[' '.join(cell.text_content().split()) for cell in row.xpath('./th|./td')] for row in table_rows]
                if not values:
                    continue
                headers = values[0]
                if headers and headers[0] == 'Port' and all(k in headers for k in ('Name', 'Signal Type', 'Optional')):
                    preceding = table.xpath('preceding::*[self::h1 or self::h2 or self::h3 or self::h4 or self::h5 or self::h6][1]')
                    section = ' '.join(preceding[0].text_content().split()) if preceding else ''
                    for row_index, row in enumerate(values[1:], 2):
                        if len(row) != len(headers):
                            raise ValueError('Independent port row has unexpected shape')
                        ports.append({'table_index': index, 'row_index': row_index, 'section': section,
                                      'port_text': row[headers.index('Port')], 'name': row[headers.index('Name')],
                                      'signal_type_text': row[headers.index('Signal Type')],
                                      'optional_text': row[headers.index('Optional')]})
                    continue
                if headers[:3] == ['Name', 'Description', 'Default']:
                    columns = [('name', 'Name'), ('default_text', 'Default'), ('unit_text', 'Units'), ('type_text', 'Type'), ('runtime_tunable_text', 'Runtime Tunable'), ('range_text', 'Range')]
                elif headers == ['Parameter', 'Description', 'Units', 'Default Value']:
                    columns = [('name', 'Parameter'), ('default_text', 'Default Value'), ('unit_text', 'Units'), ('type_text', 'Type'), ('runtime_tunable_text', 'Runtime Tunable'), ('range_text', 'Range')]
                else:
                    continue
                for row_index, row in enumerate(values[1:], 2):
                    if len(row) != len(headers):
                        raise ValueError('Independent source row has unexpected shape')
                    item = {field: row[headers.index(column)] if column in headers else None for field, column in columns}
                    item.update(table_index=index, row_index=row_index)
                    extracted.append(item)
            expected = [{k: v for k, v in p.items() if k != 'description_sha256'} for p in model['parameters']]
            if extracted != expected:
                raise ValueError('Independent parameter mismatch: ' + model['help_page'])
            if ports != model['port_tables']:
                raise ValueError('Independent port mismatch: ' + model['help_page'])
            checked += len(extracted)
            port_count += len(ports)
    if checked != records['parameter_count'] or port_count != records['explicit_port_record_count']:
        raise ValueError('Parameter count mismatch')
    return {'status': 'passed', 'method': 'independent lxml DOM row/column extraction',
            'model_pages_checked': len(records['models']), 'parameter_records_checked': checked, 'explicit_port_records_checked': port_count,
            'metadata_sha256': hashlib.sha256(metadata.read_bytes()).hexdigest(),
            'scope': 'Parameter names, defaults, units, types, tunability, ranges, table/row positions and model HTML hashes; explicit port tables; not prose, diagram port conventions or compatibility.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qch', type=Path)
    parser.add_argument('metadata', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.qch, args.metadata), ensure_ascii=False, indent=2))

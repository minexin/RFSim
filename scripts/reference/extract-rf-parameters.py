"""Extract factual model-table metadata; never infer support or evaluate defaults."""
import argparse
import hashlib
import importlib.util
import json
import os
import sqlite3
import tempfile
from contextlib import closing
from html.parser import HTMLParser
from pathlib import Path


def helper(filename):
    spec = importlib.util.spec_from_file_location(filename, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


models_tool = helper('extract-rf-models.py')
catalog_tool = helper('extract-rf-catalog.py')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def text_digest(data):
    """Fingerprint text identically in LF and CRLF Git checkouts."""
    return digest(data.replace(b"\r\n", b"\n"))


def file_digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def normalized(text):
    return ' '.join(text.split())


class Tables(HTMLParser):
    """Retain table boundaries; complex structures require explicit review."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.tables = []
        self.depth = 0
        self.row = None
        self.cell = None
        self.heading = ''
        self.heading_text = None
        self.issues = []

    def handle_starttag(self, tag, attrs):
        if tag in ('h1', 'h2', 'h3', 'h4', 'h5', 'h6') and not self.depth:
            self.heading_text = []
        if tag == 'table':
            self.depth += 1
            if self.depth == 1:
                self.tables.append({'heading': self.heading, 'rows': [], 'issues': []})
            else:
                self.tables[-1]['issues'].append('nested_table')
        if self.depth == 1:
            if tag == 'tr':
                if self.row is not None:
                    self.tables[-1]['issues'].append('unclosed_row')
                self.row = []
            if tag in ('td', 'th'):
                if self.cell is not None or self.row is None:
                    self.tables[-1]['issues'].append('malformed_cell')
                self.cell = []
                if any(value != '1' for key, value in attrs if key in ('rowspan', 'colspan')):
                    self.tables[-1]['issues'].append('spanning_cell')
            if tag == 'img' and self.cell is not None:
                self.tables[-1]['issues'].append('image_in_table')
            if tag in ('br', 'p', 'div', 'li') and self.cell is not None:
                self.cell.append(' ')

    def handle_data(self, data):
        if self.depth == 1 and self.cell is not None:
            self.cell.append(data)
        if self.heading_text is not None:
            self.heading_text.append(data)

    def handle_endtag(self, tag):
        if tag in ('h1', 'h2', 'h3', 'h4', 'h5', 'h6') and self.heading_text is not None:
            self.heading = normalized(''.join(self.heading_text))
            self.heading_text = None
        if self.depth == 1:
            if tag in ('p', 'div', 'li') and self.cell is not None:
                self.cell.append(' ')
            if tag in ('td', 'th') and self.cell is not None:
                if self.row is not None:
                    self.row.append(normalized(''.join(self.cell)))
                self.cell = None
            if tag == 'tr' and self.row is not None:
                self.tables[-1]['rows'].append(self.row)
                self.row = None
            if tag == 'table' and (self.row is not None or self.cell is not None):
                self.tables[-1]['issues'].append('unclosed_row_or_cell')
                self.row = self.cell = None
        if tag == 'table':
            self.depth = max(0, self.depth - 1)

    def finish(self):
        self.close()
        if self.depth:
            self.issues.append('unclosed_table')


PARAMETER_HEADERS = (
    ['Name', 'Description', 'Default', 'Units', 'Type', 'Runtime Tunable'],
    ['Name', 'Description', 'Default', 'Units', 'Type', 'Runtime Tunable', 'Range'],
    ['Parameter', 'Description', 'Units', 'Default Value'],
)
PORT_HEADERS = (
    ['Port', 'Name', 'Signal Type', 'Optional'],
    ['Port', 'Name', 'Description', 'Signal Type', 'Optional'],
)
FIELDS = {'name': ('Name', 'Parameter'), 'default_text': ('Default', 'Default Value'),
          'unit_text': ('Units',), 'type_text': ('Type',),
          'runtime_tunable_text': ('Runtime Tunable',), 'range_text': ('Range',)}


def parse_page(content):
    parser = Tables()
    parser.feed(content.decode('utf-8'))
    parser.finish()
    parameters, ports, sources, review = [], [], [], list(parser.issues)
    for index, table in enumerate(parser.tables, 1):
        rows = table['rows']
        if not rows:
            continue
        header = rows[0]
        kind = ('parameters' if header in PARAMETER_HEADERS else
                'ports' if header in PORT_HEADERS else None)
        if kind is None:
            # Description-only parameter tables and unfamiliar schemas are visible gaps.
            if ('Parameter' in header or 'Port' in header or
                    ('Name' in header and ('Default' in header or 'Units' in header))):
                review.append(f'table_{index}:unrecognized_metadata_header')
                sources.append({'table_index': index, 'kind': 'unrecognized', 'headers': header})
            continue
        problems = list(table['issues'])
        if len(rows) == 1:
            problems.append('empty_table')
        if any(len(row) != len(header) for row in rows[1:]):
            problems.append('row_width_mismatch')
        if any('\ufffd' in cell for row in rows for cell in row):
            problems.append('replacement_character_in_source')
        sources.append({'table_index': index, 'kind': kind, 'headers': header,
                        'data_row_count': len(rows) - 1,
                        'normalized_table_sha256': digest(json.dumps(rows, ensure_ascii=False).encode()),
                        'issues': sorted(set(problems))})
        review.extend(f'table_{index}:{issue}' for issue in sorted(set(problems)))
        # Do not partially interpret nested, spanning or malformed records.
        if any(issue != 'replacement_character_in_source' for issue in problems):
            continue
        for row_index, row in enumerate(rows[1:], 2):
            values = dict(zip(header, row))
            record = {'table_index': index, 'row_index': row_index}
            if kind == 'parameters':
                for field, candidates in FIELDS.items():
                    record[field] = next((values[key] for key in candidates if key in values), None)
                if not record['name']:
                    review.append(f'table_{index}:row_{row_index}:empty_parameter_name')
                    continue
                record['description_sha256'] = digest(values['Description'].encode())
                parameters.append(record)
            else:
                record.update({'section': table['heading'], 'port_text': values['Port'],
                               'name': values['Name'], 'signal_type_text': values['Signal Type'],
                               'optional_text': values['Optional']})
                ports.append(record)
    names = [p['name'] for p in parameters]
    if len(names) != len(set(names)):
        review.append('duplicate_parameter_names')
    if not parameters:
        review.append('no_parameter_records')
    return {'parameter_table_status': 'extracted_needs_review' if review else 'extracted',
            'parameters': parameters, 'port_tables': ports,
            'port_status': 'explicit_tables_only' if ports else 'not_found_in_explicit_tables',
            'source_tables': sources, 'manual_review': sorted(set(review)),
            'assessment_status': 'metadata_only', 'compatibility_status': 'not_assessed_here',
            'remaining_assessment': ['prose_constraints_and_enumerations', 'port_reference_conventions',
                                     'applicable_analyses', 'api_mapping', 'numerical_vendor_evidence']}


def equal_inventory(actual, expected, name):
    # The filesystem location is not the content identity.
    omit = {'source_qch'}
    if {k: v for k, v in actual.items() if k not in omit} != {
            k: v for k, v in expected.items() if k not in omit}:
        raise ValueError(name + ' does not match the current help inventory')


def extract(qch_path, catalog, inventory):
    initial_hash = file_digest(qch_path)
    equal_inventory(catalog_tool.extract(qch_path), catalog, 'Catalog')
    equal_inventory(models_tool.extract(qch_path, catalog), inventory, 'Model inventory')
    records = []
    with closing(sqlite3.connect(qch_path.resolve().as_uri() + '?mode=ro', uri=True)) as database:
        for model in inventory['models']:
            content = models_tool.read_page(database, model['help_page'])
            page_hash = digest(content)
            if page_hash != model['source_page_sha256']:
                raise ValueError('Model page changed: ' + model['help_page'])
            record = {'help_page': model['help_page'], 'names': model['names'],
                      'part_pages': model['part_pages'], 'source_page_sha256': page_hash}
            record.update(parse_page(content))
            records.append(record)
    if file_digest(qch_path) != initial_hash:
        raise ValueError('Help database changed during extraction')
    return {'schema_version': 1, 'baseline': 'SystemVue 2023 installed help',
            'source_qch_sha256': initial_hash,
            'catalog_source_sha256': catalog['source_page_sha256'],
            'coverage_meaning': 'Factual parameter and explicit port tables only; no API or vendor compatibility inference.',
            'text_policy': 'Whitespace normalized; defaults/units/ranges remain unevaluated source text. Empty cells are empty strings; absent columns are null. Descriptions are hashed, not reproduced.',
            'part_count': inventory['part_count'], 'model_count': len(records),
            'part_model_link_count': inventory['part_model_link_count'],
            'parameter_count': sum(len(r['parameters']) for r in records),
            'explicit_port_record_count': sum(len(r['port_tables']) for r in records),
            'models_needing_manual_review': [r['help_page'] for r in records if r['manual_review']],
            'models': records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qch', type=Path)
    parser.add_argument('catalog', type=Path)
    parser.add_argument('models', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    inputs = [args.qch, args.catalog, args.models, Path(__file__),
              Path(models_tool.__file__), Path(catalog_tool.__file__)]
    for source in inputs:
        if args.output.resolve() == source.resolve() or (
                args.output.exists() and os.path.samefile(args.output, source)):
            parser.error('output aliases an input or extractor')
    catalog_bytes, models_bytes = args.catalog.read_bytes(), args.models.read_bytes()
    result = extract(args.qch, json.loads(catalog_bytes), json.loads(models_bytes))
    if (args.catalog.read_bytes(), args.models.read_bytes()) != (catalog_bytes, models_bytes):
        raise ValueError('Input inventories changed during extraction')
    result['input_files'] = {'catalog_sha256': text_digest(catalog_bytes),
                             'models_sha256': text_digest(models_bytes),
                             'hash_convention': 'UTF-8 bytes with CRLF normalized to LF'}
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n',
                                         dir=args.output.parent, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(result, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
        os.replace(temporary, args.output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(json.dumps({key: result[key] for key in (
        'part_count', 'model_count', 'part_model_link_count', 'parameter_count',
        'explicit_port_record_count', 'models_needing_manual_review')}))


if __name__ == '__main__':
    main()

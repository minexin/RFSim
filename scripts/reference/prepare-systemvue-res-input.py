"""Write an owned, simulation-disabled workspace for the frozen RES experiment.

This serializes the installed application's inspected XML field names; it does
not copy an installed workspace. Live model initialization and readback are
separate, mandatory steps. A generated file is not acquisition evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET


def item(parent, name, kind):
    return ET.SubElement(parent, 'Item', XName=name, Type=kind)


def field(parent, name, value, kind=None):
    element = ET.SubElement(parent, name)
    if kind:
        ET.SubElement(element, 'T').text = kind
        ET.SubElement(element, 'D').text = str(value)
    else:
        element.text = str(value)
    return element


def param(parent, name, value, unit=None, kind=None, text=False):
    element = item(parent, name, 'Param')
    field(element, 'Data' if kind else 'DataEntry', value, kind)
    if unit is not None:
        field(element, 'Unit', unit, 'Int')
    if text:
        field(element, 'TextParam', 1, 'Bool')
    return element


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    raw_plan = args.plan.read_bytes()
    plan = json.loads(raw_plan)
    if plan['plan_id'] != 'systemvue-linear-rfsystem-minimal-20261016-v1':
        raise ValueError('This input generator implements only the identified frozen plan')
    if hashlib.sha256(raw_plan).hexdigest() != '4000c4549f5ed0a129f1f5ccf1069d4ef4c36be6da9c5de04d86b441f9d4a30e':
        raise ValueError('Frozen plan bytes changed; revise and revalidate the generator explicitly')
    args.output_dir.mkdir(parents=True, exist_ok=False)
    root = ET.Element('Item', Type='WorkspaceData')
    field(root, 'Version', '1.30')
    designs = item(root, 'Designs', 'Folder')
    design = item(designs, 'RES_Shared', 'Design')
    field(design, 'Intent', 1, 'Int')
    parts = item(design, 'PartList', 'PartList')
    topology = [('Source', 'MultiSource', '1,1'),
                ('R1', 'RES', 'Port_1,1|Port_2,2'),
                ('R2', 'RES', 'Port_1,2|Port_2,3'),
                ('Load', '*OUT', 'Port_1,3')]
    for name, model, netlist in topology:
        part = item(parts, name, 'Part')
        field(part, 'Model', model)
        field(part, 'Netlist', netlist)
        field(part, 'MathLang', 1, 'Bool')
        item(part, 'ParamSet', 'ParamSet')
    linear = item(designs, 'RES_Linear', 'Linear Analysis')
    for name in ('AutoRecalc', 'NeedRun', 'Selected'):
        field(linear, name, 0, 'Bool')
    field(linear, 'CalcNoise', 0, 'Int')
    for name, value in [('DesignName', 'RES_Shared'), ('DataName', 'RES_Linear_Data'),
                        ('ConfigurationName', ''), ('DCAnalysis', '')]:
        param(linear, name, value, text=True)
    for name, value, unit in [('Start', 100, 1003), ('Stop', 100, 1003),
                              ('Delta', 1, 1003), ('Gmin', 0, 3001),
                              ('Temperature', 26.85, 12001)]:
        param(linear, name, value, unit)
    param(linear, 'NumPoints', 1)
    param(linear, 'PrefReductionSize', 18)
    param(linear, 'List', '', 1003)
    param(linear, 'SweepType', 0, kind='Int')
    rf = item(designs, 'RES_RFSystem', 'System Analysis')
    for name in ('AutoRecalc', 'NeedRun', 'Selected', 'CalcHarmonics', 'CalcIntermods',
                 'CalcNoise', 'CalcPhaseNoise', 'ShowNoise', 'ShowPhaseNoise',
                 'ShowTotalNoise', 'ShowIntermods', 'AnalNoise', 'AnalRandNoise',
                 'AnalyzerOn', 'UseSourcePts'):
        field(rf, name, 0, 'Bool')
    for name in ('ShowSignals', 'ShowTotals', 'ShowComponents'):
        field(rf, name, 1, 'Bool')
    for name, value in [('DesignName', 'RES_Shared'), ('DataName', 'RES_RFSystem_Data'),
                        ('ConfigurationName', '')]:
        param(rf, name, value, text=True)
    for name, value, unit in [('ChanBW', 0.001, 1003), ('Temperature', 26.85, 12001),
                              ('NominalZ0', 50, 2001), ('IgnorePwrLvl', -200, 11002)]:
        param(rf, name, value, unit)
    for name, value in [('MaxOrder', 3), ('MinSourcePts', 2)]:
        param(rf, name, value)
    for name in ('UseSpecReduction', 'UsePrevSolution', 'UseCarrierRange', 'UseVolterra'):
        param(rf, name, 0, kind='Int')
    param(rf, 'DataParts', '1;4;Source;R1;R2;Load', kind='Array_BString')
    path = item(rf, 'Path0', 'PathData')
    field(path, 'PathName', 'RES_Path')
    field(path, 'PathDef', 'Source,Load')
    field(path, 'PathType', 1, 'Int')
    field(path, 'Type', 0, 'Int')
    param(path, 'Definition', '"Source,Load"')
    param(path, 'PathFreq', 100, 1003)
    for name, value in [('Enable', 1), ('AddPwrMeas', 1), ('AddVoltMeas', 1),
                        ('CalcVolt', 1), ('Internal', 0), ('AutoDescr', 1),
                        ('DigModPath', 0), ('CalcAdj', 0), ('CalcIM', 0),
                        ('CalcImage', 0), ('CalcOffset', 0), ('ForcePathSW', 1)]:
        param(path, name, value, kind='Bool')
    ET.indent(root)
    xml = b'<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE GENESYSV1.00>\n' + ET.tostring(root, encoding='utf-8')
    wsv = args.output_dir / 'res-input-uninitialized.wsv'
    wsv.write_bytes(xml)
    receipt = {'schema_version': 1, 'status': 'uninitialized_input_not_simulated',
               'plan_id': plan['plan_id'], 'plan_sha256': hashlib.sha256(raw_plan).hexdigest(),
               'input_sha256': hashlib.sha256(xml).hexdigest(),
               'physical_node_mapping': {'IN': '1', 'MID': '2', 'OUT': '3', 'ground': '0'},
               'parts': topology, 'analysis_requests': 0,
               'unresolved_before_run': ['Initialize all part models and set/read units',
                  'Confirm PathType, Type and CalcVolt meanings', 'Confirm actual topology and all analysis settings']}
    (args.output_dir / 'input-generation.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()

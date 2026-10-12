"""Offline ATTN_Linear evidence extraction and fixed-threshold replay.

Never imports COM or starts SystemVue. The extract command reads saved files;
replay runs only the RFModel reference_attenuator executable with a timeout.
A successful diagnostic exits zero even when vendor compatibility remains false.
"""

import argparse
from decimal import Decimal, localcontext
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import sqlite3
import subprocess
from xml.etree import ElementTree as ET
import zlib

ROOT = Path(__file__).resolve().parents[2]
PART = "rfdesign/Attenuator_Part.html"
MODEL = "rfdesign/ATTN_Linear.html"
PREFIX = "RFModel_AttenuatorNoise/Designs/"
DATASET = PREFIX + "System1_Data_Folder/System1_Sch1_Data_Path1"
SPECTRUM = PREFIX + "System1_Sch1_Data/Eqns/VarBlock/"
NAMES = ("CF", "CGAIN", "CND", "CNF", "DCP")
TOLERANCE = 1e-7
SI_K = Decimal("1.380649e-23")


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def canonical_sha(value):
    encoded = json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False, allow_nan=False
    )
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def module(name):
    path = Path(__file__).with_name(name + ".py")
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def only(items, label):
    if len(items) != 1:
        raise ValueError("Missing or ambiguous " + label)
    return items[0]


def number(value):
    if type(value) not in (int, float) or not math.isfinite(value):
        raise ValueError("Expected finite real number")
    return value


def nodes_of(value):
    return value if isinstance(value, list) else value["nodes"]


def node(nodes, path):
    return only([n for n in nodes if n["path"] == path], path)


def measurements(nodes):
    return [
        {
            key: node(nodes, DATASET + "/Eqns/VarBlock/" + name)[key]
            for key in ("path", "data", "dimensions")
        }
        for name in NAMES
    ]


def extract(archive, scan, workspace, qch):
    reference = load(ROOT / "validation/systemvue-2023-attenuator-1db.json")
    if sha(workspace) != reference["source_sha256"]:
        raise ValueError("Saved workspace differs from original reference source")
    nodes = nodes_of(load(archive))
    if measurements(nodes) != reference["measurements"]:
        raise ValueError("Archive measurements differ from original reference")
    if node(nodes, DATASET)["timestamp"] != reference["dataset_timestamp"]:
        raise ValueError("Archive dataset timestamp differs")
    r = ET.fromstring(Path(workspace).read_bytes())
    parts = {}
    wanted = {
        "Attn": ("L", "Zref", "ZIN", "ZOUT", "Ta", "ParamFreqList", "FrequencyDataName"),
        "Source": ("Freq", "Pwr", "ZO", "Ta"),
        "Port_2": ("ZO", "Ta"),
    }
    for name, fields in wanted.items():
        e = only(
            [e for e in r.iter("Item") if e.get("Type") == "Part" and e.get("XName") == name], name
        )
        params = only(e.findall("./Item[@XName='ParamSet']"), name + " ParamSet")
        parts[name] = {
            "model": e.findtext("Model"),
            "netlist": e.findtext("Netlist"),
            "parameters": {},
        }
        for field in fields:
            p = only(params.findall("./Item[@XName='" + field + "']"), field)
            parts[name]["parameters"][field] = {
                "entry": p.findtext("DataEntry"),
                "data": p.findtext("Data/D"),
                "uses_default": p.findtext("Default/D") == "1",
                "unit_code": p.findtext("Unit/D"),
            }
    if parts["Attn"]["model"] != "ATTN_Linear":
        raise ValueError("Saved part does not use ATTN_Linear")
    eq = only(
        [e.text for e in r.iter("Text") if e.text and "Boltzman" in e.text],
        "noise example equations",
    )
    constants = {}
    for name, pattern in {
        "loss_db": r"Atten\s*=\s*tune\(([^)]+)\)",
        "temperature_c": r"TempC\s*=\s*tune\(([^)]+)\)",
        "example_boltzmann": r"Boltzman\s*=\s*([0-9.eE+-]+)\s*;",
    }.items():
        constants[name] = number(float(only(re.findall(pattern, eq), name)))

    model_inventory = load(ROOT / "validation/systemvue-2023-rf-models.json")
    part_item = only([p for p in model_inventory["parts"] if p["help_page"] == PART], PART)
    model_item = only([m for m in model_inventory["models"] if m["help_page"] == MODEL], MODEL)
    if MODEL not in part_item["model_pages"] or PART not in model_item["part_pages"]:
        raise ValueError("Catalog part/model association differs")
    help_hashes = {}
    with sqlite3.connect(Path(qch).resolve().as_uri() + "?mode=ro", uri=True) as db:
        for item in [part_item, model_item]:
            page = item["help_page"]
            blob = only(
                db.execute(
                    "SELECT d.Data FROM FileNameTable f JOIN FileDataTable d "
                    "ON f.FileId=d.Id WHERE f.Name=?",
                    (page,),
                ).fetchall(),
                page,
            )[0]
            data = zlib.decompress(blob[4:])
            if len(data) != int.from_bytes(blob[:4], "big"):
                raise ValueError("Invalid Qt page length")
            digest = hashlib.sha256(data).hexdigest()
            if digest != item["source_page_sha256"]:
                raise ValueError("Installed help differs from catalog")
            help_hashes[page] = digest

    raw_scan = load(scan)
    observed = module("collect-attenuator-scan").collect(raw_scan, 1)
    original_scan = load(ROOT / "validation/systemvue-2023-attenuator-loss-scan.json")
    sample = only(
        [s for s in original_scan["samples"] if s["parameters"]["loss_db"] == 1], "1 dB scan sample"
    )
    if sample["capture_sha256"] != sha(scan):
        raise ValueError("Raw scan digest differs")
    if observed["measurements"] != sample["measurements"]:
        raise ValueError("Raw scan measurement differs")
    scan_nodes = nodes_of(raw_scan)
    runtime = {}
    for field in wanted["Attn"]:
        runtime[field] = node(scan_nodes, PREFIX + "Sch1/PartList/Attn/ParamSet/" + field)["data"]
    runtime["room_temperature_c"] = node(scan_nodes, PREFIX + "System1/RoomTemp")["data"]
    runtime["device_temperatures_c"] = node(nodes, SPECTRUM + "Ta")["data"]
    runtime["device_temperature_names"] = node(nodes, SPECTRUM + "TaElemList")["data"]

    d = {
        n["path"].removeprefix(SPECTRUM): n["data"] for n in nodes if n["path"].startswith(SPECTRUM)
    }
    ids = dict(zip(d["IDNo"], d["IDName"]))
    if len(ids) != len(d["IDNo"]) or len(d["IDNo"]) != len(d["IDName"]):
        raise ValueError("Ambiguous spectrum identities")
    terms = []
    selections = [
        ("1", "[Source_Noise:0],Source"),
        ("2", "[Source_Noise:0],Source,Attn"),
        ("2", "[Attn_Noise:1],Attn"),
        ("2", "Node Noise from 'Attn'"),
    ]
    for port, suffix in selections:
        count = len(d["F" + port])
        if any(len(d[k + port]) != count for k in ("P", "ID")) or any(
            len(d[k + port]) != 2 * count for k in ("V", "Z")
        ):
            raise ValueError("Spectrum dimension mismatch")
        for frequency in (99999999.5, 100000000.5):
            index = only(
                [
                    i
                    for i in range(count)
                    if d["F" + port][i] == frequency
                    and ids.get(d["ID" + port][i], "").endswith(suffix)
                ],
                suffix,
            )
            terms.append(
                {
                    "port": int(port),
                    "identity": ids[d["ID" + port][index]],
                    "frequency_hz": frequency,
                    "power_field": d["P" + port][index],
                    "voltage_peak": d["V" + port][2 * index : 2 * index + 2],
                    "impedance_ohms": d["Z" + port][2 * index : 2 * index + 2],
                }
            )
    return {
        "schema_version": 1,
        "kind": "offline_archive_excerpt",
        "part_help_page": PART,
        "model_help_page": MODEL,
        "help_sha256": help_hashes,
        "sources": [
            {"role": role, "path": str(p), "sha256": sha(p)}
            for role, p in [
                ("original_archive", archive),
                ("later_1db_scan", scan),
                ("saved_workspace", workspace),
                ("official_help", qch),
            ]
        ],
        "original_reference_sha256": sha(ROOT / "validation/systemvue-2023-attenuator-1db.json"),
        "original_reference_canonical_sha256": canonical_sha(reference),
        "original_dataset_timestamp": reference["dataset_timestamp"],
        "original_measurements": measurements(nodes),
        "saved_workspace_parts": parts,
        "saved_example_constants": constants,
        "runtime_readbacks": runtime,
        "spectral_terms": terms,
        "limits": [
            "Saved model/defaults are not runtime property readbacks",
            "Null Data does not establish a resolved parameter value",
            "Raw spectral P and path CND are retained as distinct reference quantities",
            "Example equation constant does not establish the simulator's internal constant",
        ],
    }


def independent_expected(loss_db=1, temperature_k=290, power_w=1e-19):
    # Decimal thermodynamic equilibrium and power ratios, independent of RFModel.
    with localcontext() as context:
        context.prec = 50
        loss = Decimal(str(loss_db))
        t, p = Decimal(str(temperature_k)), Decimal(str(power_w))
        gain = (-loss / Decimal(10) * Decimal(10).ln()).exp()
        source = SI_K * t * gain
        intrinsic = SI_K * t * (1 - gain)
        return {
            k: float(v)
            for k, v in {
                "gain": gain,
                "noise_factor": 1 + (1 / gain - 1) * t / Decimal(290),
                "output_noise_w_per_hz": source + intrinsic,
                "signal_output_w": gain * p,
                "source_output_w_per_hz": source,
                "intrinsic_output_w_per_hz": intrinsic,
            }.items()
        }


def check(actual, expected, tolerance):
    a, b = number(actual), number(expected)
    if b <= 0:
        raise ValueError("Expected positive baseline")
    error = abs(a - b) / b
    return {
        "actual": a,
        "expected": b,
        "relative_error": error,
        "relative_tolerance": tolerance,
        "passed": error <= tolerance,
    }


def analyze(reference, historical, evidence, actual):
    module("compare-attenuator").validate_reference(reference)
    if evidence["part_help_page"] != PART or evidence["model_help_page"] != MODEL:
        raise ValueError("Wrong catalog identity")
    if evidence["original_measurements"] != reference["measurements"]:
        raise ValueError("Evidence measurement mismatch")
    if evidence["original_dataset_timestamp"] != reference["dataset_timestamp"]:
        raise ValueError("Evidence timestamp mismatch")
    if evidence["saved_workspace_parts"]["Attn"]["model"] != "ATTN_Linear":
        raise ValueError("Wrong saved model")
    constants = evidence["saved_example_constants"]
    if constants["loss_db"] != 1 or abs(constants["temperature_c"] + 273.15 - 290) > 1e-10:
        raise ValueError("Wrong saved configuration")
    if evidence["runtime_readbacks"]["device_temperature_names"] != ["Attn"]:
        raise ValueError("Unexpected temperature reference")
    if evidence["runtime_readbacks"]["device_temperatures_c"] != [16.85]:
        raise ValueError("Wrong device temperature")
    read_loss = number(evidence["runtime_readbacks"]["L"])
    if abs(read_loss / (10**0.1) - 1) > 1e-12:
        raise ValueError("Loss readback is not a linear power ratio for 1 dB")
    expected = independent_expected()
    if set(actual) != {"gain", "noise_factor", "output_noise_w_per_hz", "signal_output_w"}:
        raise ValueError("Unexpected probe output fields")
    independent = {name: check(value, expected[name], 1e-13) for name, value in actual.items()}
    if not all(c["passed"] for c in independent.values()):
        raise ValueError("RFModel probe disagrees with independent SI mathematics")
    values = {m["path"].rsplit("/", 1)[-1]: m["data"] for m in reference["measurements"]}
    keys = dict(
        gain="CGAIN", noise_factor="CNF", output_noise_w_per_hz="CND", signal_output_w="DCP"
    )
    checks = []
    for name, field in keys.items():
        old = only([c for c in historical["checks"] if c["measurement"] == name], name)
        if old["relative_tolerance"] != TOLERANCE or old["systemvue"] != values[field][1]:
            raise ValueError("Historical baseline/tolerance changed")
        c = {"measurement": name, **check(actual[name], values[field][1], TOLERANCE)}
        if c["passed"] != old["passed"] or not check(actual[name], old["rfmodel"], 1e-13)["passed"]:
            raise ValueError("Historical result no longer reproduced")
        c["historical_rfmodel"] = old["rfmodel"]
        c["historical_native_absolute_delta"] = actual[name] - old["rfmodel"]
        c["historical_relative_error"] = old["relative_error"]
        checks.append(c)
    if historical["passed"] is not False or len(historical["checks"]) != 4:
        raise ValueError("Unexpected historical compatibility state")
    ni, no = values["CND"]
    g = expected["gain"]
    si = expected["output_noise_w_per_hz"]
    source_only = g * ni + expected["intrinsic_output_w_per_hz"]
    example_k = number(constants["example_boltzmann"])
    if example_k <= 0:
        raise ValueError("Invalid example constant")
    terms = evidence["spectral_terms"]
    spectral_checks = []
    for term in terms:
        v = complex(*term["voltage_peak"])
        z = complex(*term["impedance_ohms"])
        if not all(math.isfinite(x) for x in [v.real, v.imag, z.real, z.imag]) or z.real <= 0:
            raise ValueError("Invalid archived V/Z")
        calculated = 0.5 * abs(v) ** 2 * (1 / z).real
        c = check(term["power_field"], calculated, 1e-13)
        if not c["passed"]:
            raise ValueError("Archived spectral P differs from peak V/Z power")
        spectral_checks.append(
            {
                "identity": term["identity"],
                "port": term["port"],
                "frequency_hz": term["frequency_hz"],
                **c,
            }
        )
    budgets = []
    for freq in (99999999.5, 100000000.5):

        def power(port, suffix):
            return number(
                only(
                    [
                        t["power_field"]
                        for t in terms
                        if t["port"] == port
                        and t["frequency_hz"] == freq
                        and t["identity"].endswith(suffix)
                    ],
                    suffix,
                )
            )

        pin = power(1, "[Source_Noise:0],Source")
        source = power(2, "[Source_Noise:0],Source,Attn")
        added = power(2, "[Attn_Noise:1],Attn")
        total = power(2, "Node Noise from 'Attn'")
        summed = check(source + added, total, 1e-13)
        if not summed["passed"]:
            raise ValueError("Raw noise components do not sum to raw total")
        budgets.append(
            {
                "frequency_hz": freq,
                "raw_input_source": pin,
                "raw_output_source": source,
                "raw_output_intrinsic": added,
                "raw_output_total": total,
                "sum_check": summed,
                "source_relative_to_si_budget": source / expected["source_output_w_per_hz"] - 1,
                "intrinsic_relative_to_si_budget": added / expected["intrinsic_output_w_per_hz"]
                - 1,
                "raw_source_transfer_over_ideal": source / pin / g - 1,
                "path_CND_over_raw_total": no / total - 1,
            }
        )
    return {
        "schema_version": 1,
        "classification": "offline_diagnostic_only",
        "eligible_for_full_model_compatibility": False,
        "part_help_page": PART,
        "model_help_page": MODEL,
        "original_relative_tolerance": TOLERANCE,
        "vendor_subset_passed": all(c["passed"] for c in checks),
        "original_failure_preserved": True,
        "absolute_comparison": checks,
        "independent_decimal_expected": expected,
        "probe_independent_checks": independent,
        "path_noise_decomposition": {
            "source_offset_from_si_kT": ni / si - 1,
            "output_offset_from_si_kT": no / si - 1,
            "output_over_source_offset": no / ni - 1,
            "source_only_replacement_prediction": source_only,
            "source_only_remaining_relative_error": abs(no - source_only) / no,
            "inferred_added_from_path": no - g * ni,
            "inferred_added_offset_from_si": (no - g * ni) / expected["intrinsic_output_w_per_hz"]
            - 1,
            "example_constant": example_k,
            "example_constant_offset_from_si": example_k / float(SI_K) - 1,
            "example_constant_remaining_relative_error": abs(no - example_k * 290) / no,
            "interpretation": "Observed common normalization discrepancy; example constant is a diagnostic hypothesis, never substituted into core or acceptance",
        },
        "spectral_peak_voltage_power_checks": spectral_checks,
        "spectral_noise_budgets": budgets,
        "confirmed": [
            "Original three passes and absolute CND failure reproduced without tolerance changes",
            "Actual RFModel probe agrees with independent 50-digit SI thermal budget",
            "Both raw source and intrinsic output terms differ from SI predictions",
            "Correcting source noise alone leaves an above-threshold path CND residual",
            "Saved XML identifies ATTN_Linear; separate scan returns L as linear loss ratio",
        ],
        "unresolved": [
            "Simulator internal thermal constant and precise origin of remaining numeric offsets",
            "Resolved runtime Model/Zref/ZIN/ZOUT/source and load settings were not all captured",
            "Raw spectral P and path CND normalization are numerically distinct",
            "No mismatch, frequency-array, temperature-override or full model acceptance",
        ],
        "minimum_followup_data": [
            "Same-run resolved model name, Zref/ZIN/ZOUT, source/load impedance and temperatures, analysis noise settings",
            "Authoritative internal k/noise normalization definition or separately controlled source-only and intrinsic-noise captures with path CND and raw V/Z/P",
        ],
        "new_vendor_capture": False,
    }


def write(path, value, protected):
    path = Path(path)
    immutable = [
        ROOT / ("validation/" + p)
        for p in (
            "systemvue-2023-attenuator-1db.json",
            "systemvue-2023-attenuator-1db-comparison.json",
            "systemvue-2023-rf-catalog.json",
            "systemvue-2023-rf-models.json",
            "systemvue-2023-attenuator-loss-scan.json",
        )
    ]
    if path.resolve() in {Path(p).resolve() for p in [*protected, *immutable]}:
        raise ValueError("Output must not overwrite input evidence")
    encoded = json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    path.write_text(encoded, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("extract")
    for arg in ("archive", "scan", "workspace", "qch", "output"):
        p.add_argument(arg, type=Path)
    p = commands.add_parser("replay")
    for arg in ("reference", "historical", "evidence", "executable", "output"):
        p.add_argument(arg, type=Path)
    args = parser.parse_args()
    if args.command == "extract":
        value = extract(args.archive, args.scan, args.workspace, args.qch)
        protected = [args.archive, args.scan, args.workspace, args.qch]
    else:
        evidence = load(args.evidence)
        if evidence["original_reference_canonical_sha256"] != canonical_sha(load(args.reference)):
            raise ValueError("Reference hash mismatch")
        completed = subprocess.run(
            [str(args.executable.resolve()), "1", "290", "1e-19"],
            check=True,
            capture_output=True,
            text=True,
            timeout=20,
        )
        value = analyze(
            load(args.reference), load(args.historical), evidence, json.loads(completed.stdout)
        )
        protected = [args.reference, args.historical, args.evidence, args.executable]
        value["inputs"] = [{"path": str(p), "sha256": sha(p)} for p in protected]
        value["diagnostic_script_sha256"] = sha(__file__)
    write(args.output, value, protected)
    print(
        json.dumps(
            {
                "output": str(args.output),
                "kind": value.get("classification", value.get("kind")),
                "vendor_subset_passed": value.get("vendor_subset_passed"),
            }
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

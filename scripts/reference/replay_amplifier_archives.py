"""Append a versioned native replay of exactly six historical RFAMP captures.

This is not a new SystemVue run or full RFAMP validation. Historical numerical
predictions are retained for comparison only; archived measured values remain
the vendor oracle. Successful report generation does not mean vendor agreement.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from native_amplifier_replay import NativeAmplifierReplay
from native_replay import NativeReplayError, _sha256, atomic_write_report, forbid_product_imports

SINGLES = (
    ("single-compression-plus09", .9, 100, "sample"),
    ("single-compression-plus09-riso50", .9, 50, "sample"),
    ("single-compression-antenna-plus30", 30, 50, "antenna"),
)
# Frozen current Git bytes, distinct from historical raw-file and CRLF claims.
ARCHIVE_SHA256 = {
    'systemvue-2023-single-compression-plus09-capture.json': 'cd78a57e8da491c5b46fabb73ac08a19ddfd7203bd855f5e4383f7418975912d',
    'systemvue-2023-single-compression-plus09.json': 'a1a653c95ad5fa1fb3556e4b58281c8e217bbffcc38199e92af651c5f1e5ab6e',
    'systemvue-2023-single-compression-plus09-riso50-capture.json': '52ece1e965936ac4208b2b7ed81e5f1e6b1c3ffb0ef557fffc361e41ca82b824',
    'systemvue-2023-single-compression-plus09-riso50.json': '4a7bf673bb52ed2a48bc473fa863534e0cb20e07ef75857f1758e792b75aef98',
    'systemvue-2023-single-compression-antenna-plus30-capture.json': '1ad9a31e3a403b1aa691a948267d56000c6ebba8e5cd4c60a01d041dcb0a0d65',
    'systemvue-2023-single-compression-antenna-plus30.json': '42468815427c7d9b564a4c98aa2b755bb695873fd9f8df9c8be57bb336e69a63',
    'systemvue-2023-two-tone-captures.json': 'f481fb2f03d2fb00396b94a6c842076c57de4b0481f200be79f0df391310d7c2',
    'systemvue-2023-two-tone.json': '58400a5cb717f477917f2c86386b1f44f6f9407eac8ed9ce98b53c14fdded7c8',
}


def _load_checker(filename):
    spec = importlib.util.spec_from_file_location("amplifier_" + filename.replace("-", "_"), HERE / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def replay(probe, *, root=ROOT):
    forbid_product_imports()
    paths = {name: Path(root) / "validation" / name for name in ARCHIVE_SHA256}
    raw = {name: path.read_bytes() for name, path in paths.items()}
    if {name: _sha256(value) for name, value in raw.items()} != ARCHIVE_SHA256:
        raise ValueError("Frozen amplifier archive bytes changed")
    data = {name: json.loads(value.decode("utf-8-sig")) for name, value in raw.items()}
    provider = NativeAmplifierReplay(probe)
    single = _load_checker("compare-single-compression.py")
    two = _load_checker("compare-two-tone.py")
    reports, historical = {"single": [], "two_tone": []}, {"single": [], "two_tone": []}
    claims, case_ids = [], {"single": [], "two_tone": []}
    for stem, power, isolation, profile in SINGLES:
        capture_name, report_name = ("systemvue-2023-" + stem + suffix + ".json" for suffix in ("-capture", ""))
        capture, old = data[capture_name], data[report_name]
        if old["capture_sha256"] != capture["raw_capture_sha256"]:
            raise ValueError("Single historical raw-file hash claim disagrees with capture")
        reports["single"].append(single.compare(provider, capture, power,
            reverse_isolation_db=isolation, profile=profile))
        historical["single"].append(old)
        case_ids["single"].append(stem)
        claims.append({"capture": capture_name, "historical_report": report_name,
                       "available_capture_bytes_sha256": _sha256(raw[capture_name]),
                       "historical_report_capture_sha256": old["capture_sha256"],
                       "historical_hash_scope": "unavailable_original_raw_capture",
                       "raw_capture_sha256_claim": capture["raw_capture_sha256"],
                       "historical_library_sha256_claim": old["library_sha256"],
                       "original_raw_verified": False, "original_workspace_verified": False})
    captures_name, report_name = "systemvue-2023-two-tone-captures.json", "systemvue-2023-two-tone.json"
    captures, old = data[captures_name], data[report_name]
    if (len(captures) != 3 or [c["source_power_dbm_per_tone"] for c in captures] != [-30, -6, -3]
            or len(old["reports"]) != 3
            or [r["source_power_dbm_per_tone"] for r in old["reports"]] != [-30, -6, -3]):
        raise ValueError("Expected exactly the three ordered archived equal-power cases")
    reconstructed_crlf = _sha256(raw[captures_name].replace(b"\n", b"\r\n"))
    if reconstructed_crlf != old["captures_sha256"]:
        raise ValueError("Historical CRLF capture hash relation changed")
    for capture in captures:
        power = capture["source_power_dbm_per_tone"]
        reports["two_tone"].append(two.compare(provider, capture, power))
        case_ids["two_tone"].append("two-tone-" + str(power) + "-dbm-per-tone")
    historical["two_tone"] = old["reports"]
    claims.append({"capture": captures_name, "historical_report": report_name,
                   "available_capture_bytes_sha256": _sha256(raw[captures_name]),
                   "historical_report_captures_sha256": old["captures_sha256"],
                   "reconstructed_crlf_sha256": reconstructed_crlf,
                   "historical_hash_scope": "CRLF reconstructed in memory from current LF bytes",
                   "raw_capture_sha256_claims": [c["raw_capture_sha256"] for c in captures],
                   "historical_library_sha256_claim": old["library_sha256"],
                   "original_raw_verified": False, "original_workspace_verified": False})
    rows = []
    for kind in ("single", "two_tone"):
        for position, (current, previous) in enumerate(zip(reports[kind], historical[kind])):
            if current["relative_tolerance"] != previous["relative_tolerance"] or current["relative_tolerance"] != 1e-7:
                raise ValueError("Historical vendor tolerance changed")
            for category in ("checks",) if kind == "single" else ("checks", "direct_fundamental_checks"):
                expected_count = 10 if kind == "two_tone" and category == "checks" else 2
                if len(current[category]) != expected_count or len(previous[category]) != expected_count:
                    raise ValueError("Incomplete historical/current check collection")
                for index, (now, before) in enumerate(zip(current[category], previous[category])):
                    identity_keys = ("metric",) if kind == "single" else ("bin", "identity")
                    if any(now[key] != before[key] for key in identity_keys):
                        raise ValueError("Historical/current measured identity changed")
                    measured_key = "systemvue" if kind == "single" else "systemvue_power_w"
                    if now[measured_key] != before[measured_key]:
                        raise ValueError("Historical/current vendor observation changed")
                    rows.append({"case_id": case_ids[kind][position], "category": category,
                                 "index": index, "current": now, "historical": before})
    differences = [row for row in rows if row["current"] != row["historical"]]
    status_differences = [row for row in rows if row["current"]["passed"] != row["historical"]["passed"]]
    numeric_differences = [row for row in differences if any(
        row["current"].get(key) != row["historical"].get(key)
        for key in ("rfmodel", "rfmodel_power_w", "signed_relative_error"))]
    count, passed, old_passed = len(rows), sum(row["current"]["passed"] for row in rows), sum(row["historical"]["passed"] for row in rows)
    if count != 42:
        raise ValueError("Expected all 42 original vendor checks")
    manifest = provider.manifest(paths.values(), sorted(HERE.glob("*.py")))
    if manifest["available_archive_bytes_sha256"] != {str(paths[n]): _sha256(b) for n, b in raw.items()}:
        raise NativeReplayError("Archived bytes changed during replay")
    return {
        "schema": "rfmodel.amplifier-archives-replay", "schema_version": 1,
        "classification": "historical_archive_replay", "new_systemvue_analysis_calls": 0,
        "eligible_for_full_model_compatibility": False,
        "scope": "Three single-tone captures and three equal-power CW two-tone captures; 42 unchanged vendor judgments",
        "limitations": ["No new SystemVue execution or original raw/workspace byte verification",
                        "No raw Unit ID reconstruction, total-spectrum, phase, noise or reverse-feedback validation",
                        "No shared-source, cascade, saturation or limited-amplifier substitution"],
        "reports": reports, "historical_reports": historical,
        "comparison": {"rows": rows, "differences": differences,
                       "numeric_differences": numeric_differences, "status_differences": status_differences},
        "summary": {"capture_count": 6, "check_count": count, "passed_count": passed,
                    "failed_count": count - passed, "vendor_passed": passed == count,
                    "historical_passed_count": old_passed, "historical_failed_count": count - old_passed,
                    "historical_vendor_passed": old_passed == count},
        "archive_identity": claims, "native_provenance": manifest,
        "invocation": {"executable": sys.executable, "argv": list(sys.argv), "cwd": str(Path.cwd())},
        "interpretation": "The 1e-7 vendor verdict is computed without fitting. A replay gate can pass while vendor_passed remains false. Any native change is retained in the difference lists; no old verdict is forced.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-probe", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    report = replay(args.native_probe)
    atomic_write_report(args.report, report, protected_paths=[args.native_probe,
        str(args.native_probe) + ".manifest.json", *[ROOT / "validation" / name for name in ARCHIVE_SHA256],
        *HERE.glob("*.py")])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

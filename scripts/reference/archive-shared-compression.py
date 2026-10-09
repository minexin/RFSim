"""Archive completed shared-compression captures without accepting warned runs."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location("comparison", Path(__file__).with_name("compare-shared-compression.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def extract(raw, status, *, raw_sha256, workspace_sha256, systemvue_version):
    command = status["command"]
    if (not isinstance(command, list) or any(not isinstance(v, str) for v in command) or
            status.get("case") != "compression" or type(status.get("collector_exit_code")) is not int or
            status["collector_exit_code"] != 0):
        raise ValueError("Expected completed compression collector run")

    def switch(name):
        if command.count(name) > 1:
            raise ValueError("Duplicate switch")
        return name in command

    def value(name, default=None):
        if command.count(name) != 1:
            if not command.count(name) and default is not None:
                return default
            raise ValueError("Expected explicit unique " + name)
        index = command.index(name) + 1
        if index == len(command):
            raise ValueError("Missing value")
        return float(command[index])

    if not switch("-RunCompressionAnalysis") or not switch("-CompressionTwoTone"):
        raise ValueError("Expected controlled two-source command")
    if not switch("-CompressionShowTotals"):
        raise ValueError("Expected explicit direction totals")
    diagnostic = switch("-PreserveCancellationMessages")
    state = "captured_diagnostic" if diagnostic else "captured"
    if status.get("state") != state or status.get("eligible_for_compatibility") is not (not diagnostic):
        raise ValueError("Capture eligibility disagrees with diagnostic command")
    power = value("-SourcePowerDbm")
    spectrum = comparison.two.BASE + comparison.two.SPECTRUM
    keep_vectors = {"F2", "P2", "ID2", "V2", "Z2", "IDNo", "IDName", "RFElemList", "RFPwrIn"}
    nodes, settings = [], []
    for n in raw["nodes"]:
        path = n["path"]
        if path == comparison.two.BASE + "System1":
            settings.append(n.get("analysis_settings"))
        if ("/Sch1/PartList/" in path or path == comparison.two.BASE + "System1/Path0/PathFreq" or
                path in {spectrum + v for v in keep_vectors} or
                path == comparison.two.BASE + "System1_Data_Folder/System1_Data_Path1/Eqns/VarBlock/CF" or
                path in {comparison.two.BASE + "System1_Data",
                         comparison.two.BASE + "System1_Data_Folder/System1_Data_Path1"}):
            nodes.append({k: n.get(k) for k in ("path", "data", "dimensions", "timestamp", "evaluation_error")})
    if len(settings) != 1 or settings[0] is None:
        raise ValueError("Missing unique analysis settings")
    record = {k: raw[k] for k in ("run_started_utc", "run_returned_utc", "manager_errors",
                                 "script_language", "submitted_script")}
    record.update(nodes=nodes, analysis_settings=settings[0],
                  source_powers_dbm=[power, value("-CompressionSecondPowerDbm", power)],
                  source_phases_deg=[value("-CompressionFirstPhaseDeg"), value("-CompressionSecondPhaseDeg")],
                  same_frequency=switch("-CompressionSameFrequency"), locked=switch("-CompressionLocked"),
                  diagnostic=diagnostic, noise_enabled=not switch("-CompressionDisableNoise"),
                  systemvue_version=systemvue_version,
                  raw_capture_sha256=raw_sha256, source_workspace_sha256=workspace_sha256,
                  workspace_hash_scope="on_disk_source_copy_at_archive_time")
    comparison.inspect(record)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", type=Path, nargs="+")
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--systemvue-version", required=True)
    args = parser.parse_args()
    protected = {args.workspace.resolve()}
    for run in args.runs:
        protected.update((run / name).resolve() for name in ("status.json", "capture.json"))
    if args.output.resolve() in protected:
        parser.error("Output must not overwrite inputs")
    records, seen = [], set()
    workspace_hash = hashlib.sha256(args.workspace.read_bytes()).hexdigest()
    for run in args.runs:
        raw = (run / "capture.json").read_bytes()
        status = json.loads((run / "status.json").read_text(encoding="utf-8-sig"))
        command = status["command"]
        if command.count("-WorkspacePath") != 1:
            raise ValueError("Expected unique workspace flag")
        index = command.index("-WorkspacePath") + 1
        if index >= len(command) or Path(command[index]).resolve() != args.workspace.resolve():
            raise ValueError("Workspace differs from collector command")
        record = extract(json.loads(raw.decode("utf-8-sig")), status,
                         raw_sha256=hashlib.sha256(raw).hexdigest(), workspace_sha256=workspace_hash,
                         systemvue_version=args.systemvue_version)
        key = (tuple(record["source_powers_dbm"]), tuple(record["source_phases_deg"]),
             record["locked"], record["same_frequency"], record["noise_enabled"])
        if key in seen:
            raise ValueError("Duplicate capture configuration")
        seen.add(key)
        records.append(record)
    args.output.write_text(json.dumps(records, indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

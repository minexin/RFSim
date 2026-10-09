"""Archive successful coherent captures with command/readback validation and raw hashes."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "coherent_comparison", Path(__file__).with_name("compare-coherent-network.py"))
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def extract(raw, status, *, raw_sha256, workspace_sha256, systemvue_version):
    if (status.get("case") != "coherent" or status.get("state") != "captured" or
            type(status.get("collector_exit_code")) is not int or status["collector_exit_code"] != 0 or
            status.get("eligible_for_compatibility") is not True):
        raise ValueError("Only completed, successful coherent collector runs may be archived")
    command = status["command"]
    if not isinstance(command, list) or any(not isinstance(v, str) for v in command):
        raise ValueError("Invalid recorded command")
    if command.count("-RunCoherentAnalysis") != 1 or command.count("-CoherentLocked") > 1:
        raise ValueError("Expected one coherent analysis command")

    def flag(name):
        if command.count(name) != 1:
            raise ValueError("An explicit unique " + name + " is required")
        index = command.index(name) + 1
        if index == len(command):
            raise ValueError("Missing flag value")
        return float(command[index])

    variables = {"F3", "P3", "ID3", "V3", "Z3", "IDNo", "IDName", "RFElemList", "RFPwrIn"}
    nodes = []
    for node in raw["nodes"]:
        path = node["path"]
        keep = (path == comparison.BASE + "System1_Data" or
                (path.startswith(comparison.BASE + "Example/PartList/") and
                 "/ParamSet/" in path and node.get("data") is not None) or
                path in {comparison.VARS + name for name in variables})
        if keep:
            nodes.append({key: node.get(key) for key in
                          ("path", "data", "dimensions", "timestamp", "data_entry")})
    result = {key: raw[key] for key in ("run_started_utc", "run_returned_utc", "manager_errors")}
    result.update(nodes=nodes, locked="-CoherentLocked" in command,
                  second_phase_deg=flag("-CoherentPhaseDeg"),
                  line_length_rad=flag("-CoherentLengthRad"),
                  raw_capture_sha256=raw_sha256, source_workspace_sha256=workspace_sha256,
                  systemvue_version=systemvue_version,
                  workspace_hash_scope="on_disk_source_copy_at_archive_time")
    # Check both intent (command) and independently read actual parameter values.
    comparison.inspect(result)
    settings = [node.get("analysis_settings") for node in raw["nodes"]
                if node["path"] == comparison.BASE + "System1"]
    if len(settings) == 1 and settings[0] is not None:
        result["analysis_settings"] = settings[0]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", type=Path, nargs="+")
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--systemvue-version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    protected = {args.workspace.resolve()}
    for run in args.runs:
        protected.update((run / name).resolve() for name in ("capture.json", "status.json"))
    if args.output.resolve() in protected:
        parser.error("Output must not overwrite capture inputs or workspace")
    workspace_hash = hashlib.sha256(args.workspace.read_bytes()).hexdigest()
    records, seen = [], set()
    for run in args.runs:
        raw = (run / "capture.json").read_bytes()
        status = json.loads((run / "status.json").read_text(encoding="utf-8-sig"))
        command = status["command"]
        if command.count("-WorkspacePath") != 1:
            raise ValueError("Expected one recorded workspace path")
        index = command.index("-WorkspacePath") + 1
        if index >= len(command) or Path(command[index]).resolve() != args.workspace.resolve():
            raise ValueError("Archive workspace differs from recorded collector workspace")
        record = extract(json.loads(raw.decode("utf-8-sig")), status,
                         raw_sha256=hashlib.sha256(raw).hexdigest(),
                         workspace_sha256=workspace_hash, systemvue_version=args.systemvue_version)
        key = record["locked"], record["second_phase_deg"], record["line_length_rad"]
        if key in seen:
            raise ValueError("Duplicate reference configuration")
        seen.add(key)
        records.append(record)
    args.output.write_text(json.dumps(records, indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

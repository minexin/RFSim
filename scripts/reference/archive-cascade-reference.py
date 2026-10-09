"""Archive selected numeric cascade evidence; never distribute vendor workspaces."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path, PureWindowsPath

spec = importlib.util.spec_from_file_location(
    "cascade", Path(__file__).with_name("compare-cascade-amplifier.py")
)
comparison = importlib.util.module_from_spec(spec)
spec.loader.exec_module(comparison)


def extract(raw, status, digest):
    if (
        status.get("case") != "cascade"
        or status.get("state") != "captured"
        or status.get("collector_exit_code") != 0
        or status.get("eligible_for_compatibility") is not True
    ):
        raise ValueError("Only completed warning-free cascade captures can be archived")
    command = status["command"]

    def argument(flag):
        if command.count(flag) != 1:
            raise ValueError("Missing/duplicate command option: " + flag)
        return command[command.index(flag) + 1]

    for flag, key in (
        ("-SourcePowerDbm", "source_power_dbm"),
        ("-SourcePhaseDeg", "source_phase_deg"),
        ("-MaximumOrder", "maximum_order"),
        ("-SecondGainDb", "second_gain_db"),
        ("-ChannelBandwidthHz", "channel_bandwidth_hz"),
        ("-SecondaryRangeDb", "secondary_range_db"),
        ("-ReverseIsolationDb", "reverse_isolation_db"),
    ):
        if float(argument(flag)) != raw[key]:
            raise ValueError("Command disagrees with captured setting")
    for flag, key in (("-SecondarySpectrum", "secondary_spectrum"), ("-TwoTone", "two_tone")):
        if command.count(flag) != int(raw[key]):
            raise ValueError("Command switch disagrees with capture")
    if command.count("-DisableSpectrumReduction") != int(not raw.get("spectrum_reduction", True)):
        raise ValueError("Command disagrees with spectrum reduction")
    second_power = raw.get("second_source_power_dbm", raw["source_power_dbm"])
    submitted_second_power = (
        float(argument("-SecondSourcePowerDbm"))
        if "-SecondSourcePowerDbm" in command
        else raw["source_power_dbm"]
    )
    if submitted_second_power != second_power:
        raise ValueError("Command disagrees with second source power")
    # Collector commands always contain Windows paths, including during offline replay.
    protected = PureWindowsPath(argument("-WorkspacePath"))
    if (
        protected.name != "RFModel_CascadeIntermods.wsv"
        or protected.parent.name != "build-reference"
    ):
        raise ValueError("Unexpected workspace command")
    capture = dict(raw, raw_capture_sha256=digest)
    # Keep only controlled parameters, circuit identity, selected datasets and RF vectors.
    exact = {
        comparison.ROOT + "System3",
        comparison.ROOT + "System3_Design3_Data",
        comparison.ROOT + "System3_Data_Folder/System3_Design3_Data_Path1",
    }
    _, parameters = comparison.inspect(capture)
    exact.update(comparison.ROOT + k for k in parameters)
    exact.update(comparison.ROOT + "Design3/PartList/" + name for name in comparison.TOPOLOGY)
    variables = ["IDNo", "IDName", "RFElemList", "RFPwrIn"]
    variables += [k + str(port) for k in ("F", "P", "ID", "V", "Z") for port in (2, 3)]
    exact.update(comparison.ROOT + comparison.SPECTRUM + k for k in variables)
    capture["nodes"] = [n for n in raw["nodes"] if n["path"] in exact]
    comparison.inspect(capture)
    return capture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    inputs = [p / name for p in args.runs for name in ("capture.json", "status.json")]
    if args.output.resolve() in {p.resolve() for p in inputs}:
        parser.error("Output must not overwrite capture inputs")
    captures, seen = [], set()
    for directory in args.runs:
        raw = (directory / "capture.json").read_bytes()
        capture = extract(
            json.loads(raw.decode("utf-8-sig")),
            json.loads((directory / "status.json").read_text()),
            hashlib.sha256(raw).hexdigest(),
        )
        key = tuple(capture[k] for k in comparison.META) + (
            capture.get("second_source_power_dbm", capture["source_power_dbm"]),
            capture.get("spectrum_reduction", True),
        )
        if key in seen:
            raise ValueError("Duplicate configuration")
        seen.add(key)
        captures.append(capture)
    args.output.write_text(json.dumps(captures, indent=2, allow_nan=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

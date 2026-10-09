"""Run the guarded COM collector without UI automation or automatic retries."""
import argparse
import json
import math
import os
import re
from pathlib import Path
import subprocess
import sys
from datetime import datetime, timezone


DATASETS = {
    "coherent": ("RFModel_PhaseCombiner", "Phase Prj", "System1_Data"),
    "compression": ("RFModel_AmplifierCompression", "Designs", "System1_Data_Path1"),
    "attenuator": ("RFModel_AttenuatorNoise", "Designs", "System1_Sch1_Data_Path1"),
    "antenna": ("RFModel_AntennaNoise", "RF Design", "System1_Data_Path1"),
}


def validate_capture(capture, case, *, allow_compression_warning=False):
    if allow_compression_warning and case != "compression":
        raise ValueError("Compression warning diagnosis requires compression case")
    name, folder, dataset = DATASETS[case]
    target = "/".join((name, folder, dataset) if case == "coherent" else
                      (name, folder, "System1_Data_Folder", dataset))
    messages = capture["manager_errors"]
    if messages:
        number = r"[+-]?\d+(?:\.\d+)?"
        warning = (r"\s*\(WARNING\) Part 'RFAmp' has been driven past the input "
                   r"(?:1 dB compression|saturation) point of "
                   + number + r" dBm\. The total input power is " + number
                   + r" dBm\. Current element gain compression is " + number
                   + r" dB\. Spectrum and measurements have less accuracy\.\s*")
        if not (allow_compression_warning and case == "compression"
                and isinstance(messages, str) and re.fullmatch(warning, messages)):
            raise ValueError("SystemVue reported analysis errors or unaccepted warnings")
    times = []
    for key in ("run_started_utc", "run_returned_utc"):
        value = capture[key]
        if not value.endswith("Z"):
            raise ValueError("Expected UTC run timestamps")
        normalized = re.sub(r"(\.\d{6})\d+Z$", r"\1Z", value)
        times.append(datetime.fromisoformat(normalized[:-1] + "+00:00").timestamp())
    nodes = [node for node in capture["nodes"] if node["path"] == target]
    if len(nodes) != 1 or times[1] < times[0]:
        raise ValueError("Missing/ambiguous dataset or reversed timestamps")
    timestamp = int(nodes[0]["timestamp"])
    if not math.floor(times[0]) <= timestamp <= math.ceil(times[1]):
        raise ValueError("Stale reference dataset")


def execute(command, output_directory, case, timeout_seconds, *, compression_diagnostic=False):
    if compression_diagnostic and case != "compression":
        raise ValueError("Compression diagnostic requires compression case")
    # Exclusive directory creation prevents replacing previous evidence.
    output_directory.mkdir(parents=True, exist_ok=False)
    status = {"case": case, "started_utc": datetime.now(timezone.utc).isoformat(),
              "command": command, "state": "starting"}
    status_path = output_directory / "status.json"

    def save():
        status_path.write_text(json.dumps(status, indent=2) + "\n", encoding="utf-8")

    save()
    try:
        with (output_directory / "capture.json").open("wb") as stdout, \
                (output_directory / "stderr.log").open("wb") as stderr:
            process = subprocess.Popen(command, stdout=stdout, stderr=stderr)
            status.update(state="running", collector_pid=process.pid)
            save()
            try:
                code = process.wait(timeout=timeout_seconds)
            except subprocess.TimeoutExpired:
                status.update(state="timeout_unresolved",
                              detail="Collector may still be running. Inspect its PID and SystemVue; do not resubmit.")
                save()
                return 124
        status["collector_exit_code"] = code
        if code:
            status["state"] = "collector_failed"
            save()
            return 1
        capture = json.loads((output_directory / "capture.json").read_text(encoding="utf-8-sig"))
        validate_capture(capture, case, allow_compression_warning=compression_diagnostic)
        status["state"] = "captured_diagnostic" if compression_diagnostic else "captured"
        status["eligible_for_compatibility"] = not compression_diagnostic
        status["detail"] = ("Diagnostic capture only; preserve warning and exclude from compatibility acceptance."
                            if compression_diagnostic else
                            "Fresh dataset captured; numeric compatibility is not yet evaluated.")
        save()
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        status.update(state="failed", detail=str(error))
        save()
        return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("case", choices=DATASETS)
    parser.add_argument("workspace", type=Path)
    parser.add_argument("output_directory", type=Path)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--coherent-phase-deg", type=float)
    parser.add_argument("--coherent-length-rad", type=float)
    parser.add_argument("--coherent-locked", action="store_true")
    parser.add_argument("--coherent-show-totals", action="store_true",
                        help="Enable direction-total spectra; omitted means explicitly disabled")
    parser.add_argument("--source-power-dbm", type=float)
    parser.add_argument("--compression-riso-db", type=int, choices=(50, 100))
    parser.add_argument("--compression-profile", choices=("sample", "antenna", "limiter"))
    parser.add_argument("--compression-opsat-dbm", type=int, choices=(22, 23, 26))
    parser.add_argument("--compression-two-tone", action="store_true",
                        help="Sample/limiter profile with 1.0/1.1 GHz CW tones; defaults to equal powers")
    parser.add_argument("--compression-second-power-dbm", type=float,
                        help="Second CW tone power; requires --compression-two-tone")
    parser.add_argument("--compression-first-phase-deg", type=float)
    parser.add_argument("--compression-second-phase-deg", type=float)
    parser.add_argument("--compression-diagnostic", action="store_true",
                        help="Preserve the known over-P1dB warning for diagnosis, never compatibility acceptance")
    parser.add_argument("--open-copy", action="store_true",
                        help="Open via official script API only when no workspace is loaded")
    args = parser.parse_args()
    if args.coherent_phase_deg is not None and (
            args.case != "coherent" or not math.isfinite(args.coherent_phase_deg)
            or not -360 <= args.coherent_phase_deg <= 360):
        parser.error("Coherent phase requires coherent case and finite degrees from -360 to 360")
    if args.coherent_length_rad is not None and (
            args.case != "coherent" or not math.isfinite(args.coherent_length_rad)
            or not 0 <= args.coherent_length_rad <= 100):
        parser.error("Coherent length requires coherent case and finite radians from 0 to 100")
    if args.coherent_show_totals and args.case != "coherent":
        parser.error("Direction-total spectra require coherent case")
    if args.coherent_locked and args.case != "coherent":
        parser.error("Coherent clock requires coherent case")
    if args.case == "coherent" and args.source_power_dbm is not None:
        parser.error("Coherent case uses two fixed 0 dBm sources")
    for phase in (args.compression_first_phase_deg, args.compression_second_phase_deg):
        if phase is not None and (not args.compression_two_tone or not math.isfinite(phase)
                                  or not -360 <= phase <= 360):
            parser.error("Source phase requires two-tone mode and a finite value from -360 to 360 degrees")
    if args.compression_second_power_dbm is not None and (
            not args.compression_two_tone or not math.isfinite(args.compression_second_power_dbm)
            or not -200 <= args.compression_second_power_dbm <= 30):
        parser.error("Second power requires two-tone mode and a finite value from -200 to 30 dBm")
    if args.compression_two_tone and (args.case != "compression" or
            args.compression_profile not in (None, "sample", "limiter") or args.compression_diagnostic or
            args.source_power_dbm is None or args.compression_opsat_dbm is not None):
        parser.error("Two-tone requires sample/limiter compression, explicit first-tone power and no diagnostic/OPSAT override")
    if args.compression_diagnostic and (args.case != "compression" or args.compression_profile not in (None, "sample")):
        parser.error("Compression diagnostic requires sample-profile compression case")
    if args.compression_riso_db is not None and args.case != "compression":
        parser.error("Reverse isolation override requires compression case")
    if args.compression_profile is not None and args.case != "compression":
        parser.error("Compression profile requires compression case")
    if args.compression_opsat_dbm is not None and (
            args.case != "compression" or args.compression_profile not in (None, "sample")):
        parser.error("Saturation override requires sample-profile compression case")
    if args.source_power_dbm is not None and (not math.isfinite(args.source_power_dbm)
                                             or not -200 <= args.source_power_dbm <= 30):
        parser.error("Source power must be finite and from -200 to 30 dBm")
    if os.name != "nt":
        parser.error("SystemVue COM collection requires Windows")
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("Timeout must be positive and finite")
    root = Path(__file__).resolve().parents[2]
    workspace = args.workspace.resolve(strict=True)
    if root / "build-reference" not in workspace.parents or workspace.stem != DATASETS[args.case][0]:
        parser.error("Expected named reference copy inside build-reference")
    command = ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "RemoteSigned",
               "-File", str(Path(__file__).with_name("inspect-reference-workspace.ps1")),
               "-WorkspacePath", str(workspace), "-CaptureRun",
               {"antenna": "-RunAntennaAnalysis", "attenuator": "-RunAttenuatorAnalysis",
                "compression": "-RunCompressionAnalysis", "coherent": "-RunCoherentAnalysis"}[args.case]]
    if args.coherent_phase_deg is not None:
        command.extend(["-CoherentPhaseDeg", str(args.coherent_phase_deg)])
    if args.coherent_length_rad is not None:
        command.extend(["-CoherentLengthRad", str(args.coherent_length_rad)])
    if args.coherent_locked:
        command.append("-CoherentLocked")
    if args.coherent_show_totals:
        command.append("-CoherentShowTotals")
    if args.open_copy:
        command.append("-OpenCopy")
    if args.source_power_dbm is not None:
        command.extend(["-SourcePowerDbm", str(args.source_power_dbm)])
    if args.compression_riso_db is not None:
        command.extend(["-CompressionRisoDb", str(args.compression_riso_db)])
    if args.compression_profile is not None:
        command.extend(["-CompressionProfile", args.compression_profile])
    if args.compression_opsat_dbm is not None:
        command.extend(["-CompressionOpsatDbm", str(args.compression_opsat_dbm)])
    if args.compression_diagnostic:
        command.append("-PreserveManagerMessages")
    if args.compression_two_tone:
        command.append("-CompressionTwoTone")
    if args.compression_second_power_dbm is not None:
        command.extend(["-CompressionSecondPowerDbm", str(args.compression_second_power_dbm)])
    for flag, phase in (("-CompressionFirstPhaseDeg", args.compression_first_phase_deg),
                        ("-CompressionSecondPhaseDeg", args.compression_second_phase_deg)):
        if phase is not None:
            command.extend([flag, str(phase)])
    return execute(command, args.output_directory.resolve(), args.case, args.timeout,
                   compression_diagnostic=args.compression_diagnostic)


if __name__ == "__main__":
    sys.exit(main())

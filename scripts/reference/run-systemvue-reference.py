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
    "cascade": ("RFModel_CascadeIntermods", "Designs", "System3_Design3_Data_Path1"),
    "coherent": ("RFModel_PhaseCombiner", "Phase Prj", "System1_Data"),
    "compression": ("RFModel_AmplifierCompression", "Designs", "System1_Data_Path1"),
    "attenuator": ("RFModel_AttenuatorNoise", "Designs", "System1_Sch1_Data_Path1"),
    "antenna": ("RFModel_AntennaNoise", "RF Design", "System1_Data_Path1"),
}


CANCELLATION_WARNING = (
    "(WARNING) The Desired Channel Power (DCP) has fallen below the 'Ignore Spectrum Below Threshold' "
    "of -200.0 dBm at the output of stage 'Source' for path 'Path1'. The signal may have been converted "
    "to an undesired signal by traveling through a leakage path. Check part for correct operation. "
    "(WARNING) No desired signal was found along path 'Path1 - Source,Out'. All measurements that depend "
    "on DCP such as GAIN and CGAIN will be zero. Generally, this occurs when channel only contains noise, "
    "undesired signals, or the path begins on a node containing no source. Enable the 'Allow Path to Begin "
    "on Internal Node' option when the path doesn't begin at a source."
)


def validate_capture(capture, case, *, allow_compression_warning=False, allow_cancellation_warning=False):
    if allow_compression_warning and allow_cancellation_warning:
        raise ValueError("Diagnostic warning modes are mutually exclusive")
    if (allow_compression_warning or allow_cancellation_warning) and case != "compression":
        raise ValueError("Compression warning diagnosis requires compression case")
    name, folder, dataset = DATASETS[case]
    target = "/".join((name, folder, dataset) if case == "coherent" else
                      (name, folder, "System3_Data_Folder" if case == "cascade" else "System1_Data_Folder", dataset))
    messages = capture["manager_errors"]
    if messages:
        number = r"[+-]?\d+(?:\.\d+)?"
        warning = (r"\s*\(WARNING\) Part 'RFAmp' has been driven past the input "
                   r"(?:1 dB compression|saturation) point of "
                   + number + r" dBm\. The total input power is " + number
                   + r" dBm\. Current element gain compression is " + number
                   + r" dB\. Spectrum and measurements have less accuracy\.\s*")
        compression_match = (allow_compression_warning and isinstance(messages, str)
                             and re.fullmatch(warning, messages))
        cancellation_match = (allow_cancellation_warning and isinstance(messages, str)
                              and " ".join(messages.split()) == CANCELLATION_WARNING)
        if not (compression_match or cancellation_match):
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


def execute(command, output_directory, case, timeout_seconds, *, compression_diagnostic=False,
            cancellation_diagnostic=False):
    if compression_diagnostic and cancellation_diagnostic:
        raise ValueError("Diagnostic modes are mutually exclusive")
    diagnostic = compression_diagnostic or cancellation_diagnostic
    if diagnostic and case != "compression":
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
        validate_capture(capture, case, allow_compression_warning=compression_diagnostic,
                         allow_cancellation_warning=cancellation_diagnostic)
        status["state"] = "captured_diagnostic" if diagnostic else "captured"
        status["eligible_for_compatibility"] = not diagnostic
        status["detail"] = ("Diagnostic capture only; preserve warning and exclude from compatibility acceptance."
                            if diagnostic else
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
    parser.add_argument("--cascade-channel-bandwidth-hz", type=float, choices=(1., 1e6))
    parser.add_argument("--cascade-two-tone", action="store_true")
    parser.add_argument("--cascade-secondary-spectrum", action="store_true")
    parser.add_argument("--cascade-secondary-range-db", type=int, choices=(-140, -50, 50, 140))
    parser.add_argument("--cascade-riso-db", type=int, choices=(100, 140))
    parser.add_argument("--cascade-max-order", type=int, choices=(2, 3, 5))
    parser.add_argument("--cascade-phase-deg", type=float)
    parser.add_argument("--cascade-second-gain-db", type=float)
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
    parser.add_argument("--compression-same-frequency", action="store_true",
                        help="Two CW sources at 1 GHz instead of the 1.0/1.1 GHz pair")
    parser.add_argument("--compression-locked", action="store_true",
                        help="Assign the same reference clock to both CW sources")
    parser.add_argument("--compression-show-totals", action="store_true")
    parser.add_argument("--compression-disable-noise", action="store_true")
    parser.add_argument("--compression-cancellation-diagnostic", action="store_true",
                        help="Preserve the known cancelled-path warnings; exclude from acceptance")
    parser.add_argument("--compression-second-power-dbm", type=float,
                        help="Second CW tone power; requires --compression-two-tone")
    parser.add_argument("--compression-first-phase-deg", type=float)
    parser.add_argument("--compression-second-phase-deg", type=float)
    parser.add_argument("--compression-diagnostic", action="store_true",
                        help="Preserve the known over-P1dB warning for diagnosis, never compatibility acceptance")
    parser.add_argument("--open-copy", action="store_true",
                        help="Open via official script API only when no workspace is loaded")
    args = parser.parse_args()
    if args.case != "cascade" and any(v is not None for v in (
            args.cascade_max_order, args.cascade_phase_deg, args.cascade_second_gain_db,
            args.cascade_channel_bandwidth_hz, args.cascade_secondary_range_db, args.cascade_riso_db)):
        parser.error("Cascade settings require cascade case")
    if (args.cascade_secondary_spectrum or args.cascade_two_tone) and args.case != "cascade":
        parser.error("Secondary spectrum setting requires cascade case")
    if args.case == "cascade":
        if (args.source_power_dbm is None or not math.isfinite(args.source_power_dbm)
                or not -60 <= args.source_power_dbm <= -10):
            parser.error("Cascade requires source power from -60 to -10 dBm")
        if args.cascade_second_gain_db is not None and (
                not math.isfinite(args.cascade_second_gain_db)
                or not -10 <= args.cascade_second_gain_db <= 20):
            parser.error("Cascade second gain must be finite and from -10 to 20 dB")
        if args.cascade_phase_deg is not None and (
                not math.isfinite(args.cascade_phase_deg) or not -180 <= args.cascade_phase_deg <= 180):
            parser.error("Cascade phase must be finite and from -180 to 180 degrees")
        if args.open_copy:
            parser.error("Cascade collector attaches only; open the protected reference copy first")
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
    if args.compression_cancellation_diagnostic and (
            not args.compression_same_frequency or not args.compression_locked or
            args.compression_diagnostic):
        parser.error("Cancellation diagnosis requires same-frequency locked two-tone input")
    if (args.compression_same_frequency or args.compression_locked) and not args.compression_two_tone:
        parser.error("Same-frequency and clock options require two-tone compression")
    if (args.compression_show_totals or args.compression_disable_noise) and args.case != "compression":
        parser.error("Compression spectrum settings require compression case")
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
    if args.case == "cascade":
        if workspace.parent != root / "build-reference":
            parser.error("Cascade copy must be directly inside build-reference")
        command = ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "RemoteSigned",
                   "-File", str(Path(__file__).with_name("capture-cascade-reference.ps1")),
                   "-WorkspacePath", str(workspace), "-SourcePowerDbm", str(args.source_power_dbm),
                   "-SourcePhaseDeg", str(args.cascade_phase_deg or 0.),
                   "-MaximumOrder", str(args.cascade_max_order or 3),
                   "-SecondGainDb", str(10 if args.cascade_second_gain_db is None else args.cascade_second_gain_db),
                   "-ChannelBandwidthHz", str(args.cascade_channel_bandwidth_hz or 1e6),
                   "-SecondaryRangeDb", str(args.cascade_secondary_range_db or -50),
                   "-ReverseIsolationDb", str(args.cascade_riso_db or 100)]
        if args.cascade_secondary_spectrum:
            command.append("-SecondarySpectrum")
        if args.cascade_two_tone:
            command.append("-TwoTone")
        return execute(command, args.output_directory.resolve(), args.case, args.timeout)
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
    if args.compression_same_frequency:
        command.append("-CompressionSameFrequency")
    if args.compression_locked:
        command.append("-CompressionLocked")
    if args.compression_show_totals:
        command.append("-CompressionShowTotals")
    if args.compression_disable_noise:
        command.append("-CompressionDisableNoise")
    if args.compression_cancellation_diagnostic:
        command.append("-PreserveCancellationMessages")
    if args.compression_second_power_dbm is not None:
        command.extend(["-CompressionSecondPowerDbm", str(args.compression_second_power_dbm)])
    for flag, phase in (("-CompressionFirstPhaseDeg", args.compression_first_phase_deg),
                        ("-CompressionSecondPhaseDeg", args.compression_second_phase_deg)):
        if phase is not None:
            command.extend([flag, str(phase)])
    return execute(command, args.output_directory.resolve(), args.case, args.timeout,
                   compression_diagnostic=args.compression_diagnostic,
                   cancellation_diagnostic=args.compression_cancellation_diagnostic)


if __name__ == "__main__":
    sys.exit(main())

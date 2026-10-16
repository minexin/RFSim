"""Build an installed-only native probe and record reproducible local evidence.

Only creates --out, which must not exist. Python is developer orchestration;
the calculation uses the installed C++17 SDK exclusively.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--cmake", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--generator")
    parser.add_argument("--architecture")
    parser.add_argument("--configuration", action="append", choices=("Debug", "Release"),
                        help="configuration to validate; repeat to select both (default: both)")
    args = parser.parse_args()
    configurations = list(dict.fromkeys(args.configuration or ("Debug", "Release")))
    repo, output, cmake = args.repo.resolve(), args.out.resolve(), args.cmake.resolve()
    probe = Path(__file__).resolve().parent
    plan = repo / "validation/systemvue-linear-rfsystem-minimal-plan-20261016.json"
    if output.exists():
        parser.error("--out must be fresh to exclude stale executables/install files")
    output.mkdir(parents=True)
    receipt = {"schema_version": 1, "status": "running",
               "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
               "repository": str(repo), "cmake": str(cmake), "python": sys.executable,
               "requested_configurations": configurations,
               "plan_sha256": sha256(plan), "commands": [], "configurations": [],
               "vendor_comparisons_performed": 0}
    receipt_path = output / "receipt.json"
    save(receipt_path, receipt)

    def invoke(name, command, stdout_path=None):
        command = [str(arg) for arg in command]
        # On Windows explicitly materialize Python's case-folded environment:
        # inherited raw Path/PATH duplicates make MSBuild reject CL.exe startup.
        process = subprocess.run(command, cwd=repo, env=dict(os.environ),
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout_path = stdout_path or output / (name + ".stdout.log")
        stderr_path = output / (name + ".stderr.log")
        stdout_path.write_bytes(process.stdout)
        stderr_path.write_bytes(process.stderr)
        receipt["commands"].append({"name": name, "argv": command, "cwd": str(repo),
                                    "returncode": process.returncode,
                                    "stdout": str(stdout_path.relative_to(output)),
                                    "stderr": str(stderr_path.relative_to(output)),
                                    "stdout_sha256": sha256(stdout_path),
                                    "stderr_sha256": sha256(stderr_path)})
        save(receipt_path, receipt)
        if process.returncode:
            raise RuntimeError(name + " failed; see local receipt/logs")
        return process.stdout

    try:
        receipt["source_commit"] = invoke("source-commit", ["git", "rev-parse", "HEAD"]).decode("ascii").strip()
        invoke("source-status", ["git", "status", "--porcelain=v1"])
        source_files = sorted(set(list((repo / "include").rglob("*.hpp")) +
                                  list((repo / "include").rglob("*.h")) +
                                  list((repo / "cmake").glob("*.cmake")) +
                                  list((repo / "cmake").glob("*.in")) +
                                  [repo / "CMakeLists.txt", plan] +
                                  [p for p in probe.iterdir() if p.is_file()]))
        sources = [{"path": str(p.relative_to(repo)), "sha256": sha256(p), "bytes": p.stat().st_size}
                   for p in source_files]
        save(output / "source-hashes.json", sources)
        generator = (["-G", args.generator] if args.generator else [])
        if args.architecture:
            generator += ["-A", args.architecture]
        sdk, producer = output / "sdk", output / "producer"
        invoke("sdk-configure", [cmake, "-S", repo, "-B", producer, *generator,
                                 "-DBUILD_TESTING=OFF", "-DRFMODEL_BUILD_C_API=OFF",
                                 "-DRFMODEL_BUILD_CLI=OFF", "-DRFMODEL_BUILD_EXAMPLES=OFF",
                                 "-DRFMODEL_ENABLE_SDK_PACKAGING=OFF", "-DCMAKE_INSTALL_LIBDIR=lib",
                                 "-DCMAKE_INSTALL_PREFIX=" + str(sdk)])
        invoke("sdk-install", [cmake, "--install", producer, "--config", configurations[0]])
        installed = [{"path": str(p.relative_to(sdk)), "sha256": sha256(p), "bytes": p.stat().st_size}
                     for p in sorted(sdk.rglob("*")) if p.is_file()]
        save(output / "installed-sdk-hashes.json", installed)
        for config in configurations:
            build = output / ("consumer-" + config.lower())
            invoke(config + "-configure", [cmake, "-S", probe, "-B", build, *generator,
                                            "-DSDK_PREFIX=" + str(sdk), "-DCMAKE_BUILD_TYPE=" + config])
            build_log = invoke(config + "-build", [cmake, "--build", build, "--config", config,
                                                    "--parallel", "2"])
            compiler_id = (build / "compiler-id.txt").read_text(encoding="utf-8").strip()
            includes = []
            if compiler_id == "MSVC":
                # /showIncludes is compiler-generated evidence, including nested
                # includes. Restrict only RFModel paths, not standard libraries.
                for line in build_log.decode("utf-8", errors="replace").splitlines():
                    match = re.search(r"([A-Za-z]:[\\/].*[\\/]rfmodel[\\/].*\.(?:hpp|h))\s*$", line)
                    if match:
                        header = Path(match.group(1)).resolve()
                        if not header.is_relative_to(sdk):
                            raise RuntimeError("RFModel include escaped installed SDK: " + str(header))
                        includes.append(str(header.relative_to(sdk)))
                if not includes:
                    raise RuntimeError("MSVC include trace absent")
                save(output / (config + "-included-rfmodel-headers.json"), sorted(set(includes)))
            executable = build / "bin" / ("behavioral_res_probe.exe" if os.name == "nt" else "behavioral_res_probe")
            result_path = output / (config + "-native.json")
            invoke(config + "-execute", [executable], result_path)
            check_path = output / (config + "-analytic-check.json")
            invoke(config + "-verify", [sys.executable, probe / "verify.py", "--plan", plan,
                                        "--result", result_path], check_path)
            result = json.loads(check_path.read_text(encoding="utf-8"))
            receipt["configurations"].append({"configuration": config, "status": result["status"],
                                               "checks_passed": result["checks_passed"],
                                               "compiler_id": compiler_id,
                                               "compiler_version": (build / "compiler-version.txt").read_text(encoding="utf-8"),
                                               "compiler_path": (build / "compiler-path.txt").read_text(encoding="utf-8"),
                                               "executable_sha256": sha256(executable),
                                               "result_sha256": sha256(result_path),
                                               "analytic_check_sha256": sha256(check_path),
                                               "installed_include_trace_count": len(set(includes))})
        # A changed source or SDK mid-run makes the receipt non-reproducible.
        for entry in sources:
            if sha256(repo / entry["path"]) != entry["sha256"]:
                raise RuntimeError("source changed during evidence run: " + entry["path"])
        for entry in installed:
            if sha256(sdk / entry["path"]) != entry["sha256"]:
                raise RuntimeError("installed SDK changed during evidence run")
        negative_path = output / "verifier-negative-checks.json"
        negative_config = configurations[-1]
        invoke("verifier-negative-checks", [sys.executable, probe / "test_verify.py", "--plan", plan,
                                           "--result", output / (negative_config + "-native.json")], negative_path)
        receipt["verifier_negative_checks_configuration"] = negative_config
        receipt["verifier_negative_checks"] = json.loads(negative_path.read_text(encoding="utf-8"))["negative_checks_passed"]
        receipt["verifier_negative_checks_sha256"] = sha256(negative_path)
        receipt["source_hashes_sha256"] = sha256(output / "source-hashes.json")
        receipt["installed_sdk_hashes_sha256"] = sha256(output / "installed-sdk-hashes.json")
        receipt["status"] = "pass"
    except Exception as error:
        receipt["status"] = "fail"
        receipt["error"] = str(error)
        raise
    finally:
        receipt["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        save(receipt_path, receipt)
    print(json.dumps({"status": receipt["status"], "receipt": str(receipt_path),
                      "configurations": receipt["configurations"]}, indent=2))


if __name__ == "__main__":
    main()

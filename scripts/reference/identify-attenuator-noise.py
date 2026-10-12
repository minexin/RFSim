"""Replay bounded non-isothermal noise identification without COM or fitted constants.

Exit zero means diagnostic evidence checks passed. Vendor acceptance remains
the original 1e-7 comparison, independently reported and never overwritten.
"""
import argparse
from decimal import Decimal, localcontext
from datetime import datetime
import hashlib
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
D = lambda x: Decimal(str(x))
SI = D("1.380649e-23")
LEGACY = D("1.3806504e-23")
SOURCE_FACTOR = 1 - D("2.5e-8")
PATH_FACTOR = 1 - D("5e-9")
IDENTIFICATION_TOLERANCE = 1e-12
VENDOR_TOLERANCE = 1e-7


def check(actual, expected, tolerance):
    a, e = D(actual), D(expected)
    if not a.is_finite() or not e.is_finite() or e <= 0:
        raise ValueError("Expected finite number and positive baseline")
    error = abs(a - e) / e
    return dict(actual=str(a), expected=str(e), relative_error=float(error),
                relative_tolerance=tolerance, passed=error <= D(tolerance))


def term(case, port, suffix, frequency=99999999.5):
    matches = [s for s in case["spectral_terms"] if s["port"] == port
               and s["identity"].endswith(suffix) and s["frequency_hz"] == frequency]
    if len(matches) != 1:
        raise ValueError("Missing or ambiguous spectrum identity")
    return D(matches[0]["power_field"])


def analyze(evidence):
    with localcontext() as ctx:
        ctx.prec = 50
        gain = D(10) ** D("-.1")
        cases = evidence["cases"]
        if [c["device_temperature_k"] for c in cases] != [290, 300, 350]:
            raise ValueError("Unexpected experimental design")
        predictions = evidence["preregistered_predictions"]
        if predictions["original_compatibility_tolerance"] != VENDOR_TOLERANCE:
            raise ValueError("Changed vendor tolerance")
        out = []
        for case in cases:
            t = D(case["device_temperature_k"])
            start = datetime.fromisoformat(case["run_started_utc"].replace("Z", "+00:00")).timestamp()
            finish = datetime.fromisoformat((case["run_returned_utc"] or case["completion_observed_utc"]).replace("Z", "+00:00")).timestamp()
            for stamp in ("dataset_timestamp", "spectrum_timestamp"):
                if not math.floor(start) <= int(case[stamp]) <= math.ceil(finish):
                    raise ValueError("Dataset timestamp outside observed analysis interval")
            r = case["runtime"]
            if case["manager_errors"] or case["temperature_names"] != ["Attn"]:
                raise ValueError("Wrong temperature identity or Manager errors")
            if abs(D(case["effective_device_temperature_c"][0]) + D("273.15") - t) > D("1e-10"):
                raise ValueError("Effective device temperature differs")
            if abs(D(r["System1/RoomTemp"]["data"]) + D("273.15") - 290) > D("1e-10"):
                raise ValueError("Source/global temperature changed")
            if r["Sch1/PartList/Attn/ParamSet/L"]["data_entry"] != "1":
                raise ValueError("Loss changed")
            if r["Sch1/PartList/Source/ParamSet/Pwr"]["data_entry"] != "-160":
                raise ValueError("Source power changed")
            if case["measurements"]["CF"] != [1e8, 1e8]:
                raise ValueError("Frequency changed")
            if t != 290:
                if datetime.fromisoformat(predictions["registered_utc"]) >= datetime.fromisoformat(case["run_started_utc"].replace("Z", "+00:00")):
                    raise ValueError("Prediction was not registered before experiment")
                restored = case["restored_parameters"]
                for name in r:
                    if any(restored[name][f] != cases[0]["runtime"][name][f]
                           for f in ("data", "data_entry", "evaluation_error")):
                        raise ValueError("Restoration differs: " + name)
            src = term(case, 2, "[Source_Noise:0],Source,Attn")
            device = term(case, 2, "[Attn_Noise:1],Attn")
            total = term(case, 2, "Node Noise from 'Attn'")
            path = D(case["measurements"]["CND"][1])
            checks = {
                "source_invariant": check(src, term(cases[0], 2, "[Source_Noise:0],Source,Attn"), IDENTIFICATION_TOLERANCE),
                "source_legacy_prediction": check(src, LEGACY * 290 * gain * SOURCE_FACTOR, IDENTIFICATION_TOLERANCE),
                "device_legacy_prediction": check(device, LEGACY * t * (1-gain), IDENTIFICATION_TOLERANCE),
                "spectrum_component_sum": check(total, src + device, IDENTIFICATION_TOLERANCE),
                "path_normalization": check(path, total * PATH_FACTOR, IDENTIFICATION_TOLERANCE),
            }
            for i, s in enumerate(case["spectral_terms"]):
                vr, vi = map(D, s["voltage_peak"])
                zr, zi = map(D, s["impedance_ohms"])
                checks["spectrum_voltage_power_" + str(i)] = check(
                    s["power_field"], (vr*vr+vi*vi)*zr/(zr*zr+zi*zi)/2, IDENTIFICATION_TOLERANCE)
            if t != 290:
                p = next(p for p in predictions["cases"] if p["device_temperature_k"] == int(t))
                for key, actual in [("raw_source_out", src), ("raw_device_out", device), ("path_cnd", path)]:
                    checks["preregistered_" + key] = check(actual, p[key], IDENTIFICATION_TOLERANCE)
            candidates = {str(k): check(device, k*t*(1-gain), IDENTIFICATION_TOLERANCE)
                          for k in (SI, D("1.3806503e-23"), LEGACY)}
            si_noise = SI * (290*gain+t*(1-gain))
            source_only = (src + SI*t*(1-gain)) * PATH_FACTOR
            out.append(dict(device_temperature_k=int(t),checks=checks,device_constant_candidates=candidates,
                            inferred_device_coefficient=str(device/t/(1-gain)),
                            path_to_raw_total=str(path/total),
                            si_output_noise_comparison=check(si_noise,path,VENDOR_TOLERANCE),
                            source_corrected_only_comparison=check(source_only,path,VENDOR_TOLERANCE)))
        c300,c350 = cases[1:]
        slope = (term(c350,2,"[Attn_Noise:1],Attn")-term(c300,2,"[Attn_Noise:1],Attn"))/50/(1-gain)
        return dict(schema_version=1,diagnostic_only=True,core_constant_changed=False,
                    vendor_tolerance=VENDOR_TOLERANCE,identification_tolerance=IDENTIFICATION_TOLERANCE,
                    diagnostic_passed=all(v["passed"] for c in out for v in c["checks"].values()),
                    vendor_noise_subset_passed=all(c["si_output_noise_comparison"]["passed"] for c in out),
                    independent_device_slope_coefficient=str(slope),
                    slope_candidates={str(k):check(slope,k,IDENTIFICATION_TOLERANCE) for k in (SI,D("1.3806503e-23"),LEGACY)},
                    cases=out,
                    conclusion="Effective device coefficient and pre-registered decomposition reproduced; literal Spectrasys internal constant and numerical regularization mechanism remain unproved.")


def historical_crosschecks():
    """Retrospective scope checks, never mislabeled as preregistered holdouts."""
    rows=[]
    with localcontext() as ctx:
        ctx.prec=50
        for name in ("temperature", "loss-scan", "near-zero", "power"):
            path=ROOT / ("validation/systemvue-2023-attenuator-"+name+".json")
            for sample in json.loads(path.read_text(encoding="utf-8-sig"))["samples"]:
                p=sample["parameters"]
                g=D(10)**(-D(p["loss_db"])/10)
                predicted=LEGACY*D(p["temperature_k"])*(g*SOURCE_FACTOR+1-g)*PATH_FACTOR
                actual=next(x["data"][1] for x in sample["measurements"] if x["path"].endswith("/CND"))
                rows.append(dict(source=path.name,parameters=p,
                                 check=check(actual,predicted,IDENTIFICATION_TOLERANCE)))
    return rows


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("evidence",type=Path)
    p.add_argument("output",type=Path)
    a=p.parse_args()
    if a.output.exists():
        raise ValueError("Refusing to overwrite output")
    result=analyze(json.loads(a.evidence.read_text(encoding="utf-8-sig")))
    result["historical_crosschecks"]=historical_crosschecks()
    result["evidence_sha256"]=hashlib.sha256(a.evidence.read_bytes()).hexdigest()
    a.output.write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    print(json.dumps({k:result[k] for k in ["diagnostic_passed","vendor_noise_subset_passed","independent_device_slope_coefficient"]}))
    return 0 if result["diagnostic_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

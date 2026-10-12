"""Offline near-zero loss diagnosis. Never invokes SystemVue or changes acceptance.

The extract subcommand reads eight explicitly archived cases into a compact
evidence file. Replay distinguishes rejected preregistered hypotheses from
post-hoc algebraic reconstruction; neither promotes vendor compatibility.
"""
import argparse
from datetime import datetime
from decimal import Decimal, localcontext
import hashlib
import importlib.util
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PREFIX = "RFModel_AttenuatorNoise/Designs/"
PATH = PREFIX + "System1_Data_Folder/System1_Sch1_Data_Path1"
SPECTRUM = PREFIX + "System1_Sch1_Data"
D = lambda value: Decimal(str(value))
K = D("1.3806504e-23")
CHECK_TOL = 1e-12
NEW_CASES = [
    ("attenuator-near-zero-3e-06dB-20261012T052620Z-0bc58366", "0.000003"),
    ("attenuator-near-zero-6e-06dB-20261012T052634Z-9207ba1c", "0.000006"),
]


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def only(values):
    if len(values) != 1:
        raise ValueError("Missing or ambiguous evidence")
    return values[0]


def nodes(capture):
    values = capture["nodes"]
    # Restrict uniqueness to paths used below; source has unrelated duplicate names.
    def node(path):
        return only([n for n in values if n["path"] == path])
    return node


def parameters(capture):
    get = nodes(capture)
    names = ["System1/RoomTemp"]
    for part, fields in [
        ("Attn", ["L", "Ta", "Zref", "ZIN", "ZOUT"]),
        ("Source", ["Freq", "Pwr", "R", "Ta"]),
        ("Port_2", ["ZO", "Ta"]),
    ]:
        names += ["Sch1/PartList/" + part + "/ParamSet/" + f for f in fields]
    result = {}
    for name in names:
        present = [n for n in capture["nodes"] if n["path"] == PREFIX+name]
        if not present:
            result[name] = {"captured": False}
        else:
            result[name] = {f: only(present).get(f) for f in
                            ("data", "data_entry", "data_source", "evaluation_error")}
    return result


def extract_case(path, loss, fresh):
    capture = load(path)
    get = nodes(capture)
    def spectrum(name):
        return get(SPECTRUM + "/Eqns/VarBlock/" + name)["data"]
    ids = dict(zip(spectrum("IDNo"), spectrum("IDName")))
    terms = []
    selections = [
        (1, "source_in", "[Source_Noise:0],Source"),
        (2, "source_out", "[Source_Noise:0],Source,Attn"),
        (2, "device_out", "[Attn_Noise:1],Attn"),
        (2, "total_out", "Node Noise from 'Attn'"),
    ]
    for port, role, suffix in selections:
        port_text = str(port)
        f, ident, p, v, z = [spectrum(k+port_text) for k in ("F", "ID", "P", "V", "Z")]
        if len(f) != len(ident) or len(f) != len(p) or len(v) != 2*len(f) or len(z) != 2*len(f):
            raise ValueError("Wrong raw spectrum dimensions")
        for frequency in (99999999.5, 100000000.5):
            indexes = [i for i in range(len(f)) if f[i] == frequency
                       and ids[ident[i]].endswith(suffix)]
            if not indexes and role == "device_out":
                # Absence in an exported spectrum is not proof of physically zero noise.
                terms.append(dict(role=role, frequency_hz=frequency, exported=False))
                continue
            i = only(indexes)
            terms.append(dict(role=role, port=port, frequency_hz=frequency, exported=True,
                              identity=ids[ident[i]], power=p[i], voltage=v[2*i:2*i+2],
                              impedance=z[2*i:2*i+2]))
    measurements = {name: get(PATH+"/Eqns/VarBlock/"+name)["data"]
                    for name in ("CF", "CGAIN", "CNF", "CND", "DCP", "ZIN", "ZOUT")}
    result = dict(loss_db=loss, new_case=fresh, capture_path=path.relative_to(ROOT).as_posix(),
                  capture_sha256=sha(path), run_started_utc=capture["run_started_utc"],
                  run_returned_utc=capture["run_returned_utc"], manager_errors=capture["manager_errors"],
                  dataset_timestamp=get(PATH)["timestamp"], spectrum_timestamp=get(SPECTRUM)["timestamp"],
                  parameters=parameters(capture), effective_device_temperature_c=spectrum("Ta"),
                  measurements=measurements, spectral_terms=terms)
    if fresh:
        result["before"] = parameters(load(path.parent/"before.json"))
        result["restored"] = parameters(load(path.parent/"restored.json"))
        result["restored_sha256"] = sha(path.parent/"restored.json")
        result["collection_status"] = load(path.parent/"status.json")
    return result


def extract():
    cases = []
    for loss in ("0", "0.00000001", "0.000001", "0.0001", "0.01", "1"):
        path = ROOT / ("build-reference/near-zero/attenuator-scan-"+loss+".json")
        cases.append(extract_case(path, loss, False))
    for directory, loss in NEW_CASES:
        cases.append(extract_case(ROOT/"build-reference"/directory/"capture.json", loss, True))
    base = ROOT/"build-reference/near-zero-identification-20261012"
    return dict(schema_version=1,kind="near_zero_identification_excerpt",
                preregistered_predictions=load(base/"preregistered-predictions.json"),
                preregistration_sha256=sha(base/"preregistered-predictions.json"),
                cases=cases, limits=[
                    "Two fresh runs only; six historical cases were read without rerun.",
                    "Reconstruction based on measured spectra is not an independent prediction.",
                    "Missing exported device spectrum does not establish physically zero noise.",
                    "Empty default impedance parameters do not establish resolved model defaults."])


def relative(actual, expected):
    actual, expected = D(actual), D(expected)
    if not actual.is_finite() or not expected.is_finite() or expected <= 0:
        raise ValueError("Expected finite data and positive denominator")
    return float((actual-expected)/expected)


def checked(actual, expected):
    error = relative(actual, expected)
    return dict(actual=str(actual), expected=str(expected),
                signed_relative_error=error, relative_tolerance=CHECK_TOL,
                passed=abs(error) <= CHECK_TOL)


def voltage_power(term, reference=None):
    vr,vi = map(D,term["voltage"])
    zr,zi = map(D,term["impedance"])
    if zr <= 0:
        raise ValueError("Nonpositive spectral resistance")
    admittance = zr/(zr*zr+zi*zi) if reference is None else 1/D(reference)
    return (vr*vr+vi*vi)*admittance/2


def analyze(evidence):
    with localcontext() as context:
        context.prec=50
        fresh = [c for c in evidence["cases"] if c["new_case"]]
        if [c["loss_db"] for c in fresh] != ["0.000003","0.000006"]:
            raise ValueError("Unexpected new point set")
        registered = evidence["preregistered_predictions"]
        if registered["original_vendor_tolerance"] != 1e-7:
            raise ValueError("Vendor tolerance changed")
        rows=[]
        for case in evidence["cases"]:
            loss = D(case["loss_db"]); g = D(10)**(-loss/10)
            m = case["measurements"]; p = case["parameters"]
            start=datetime.fromisoformat(case["run_started_utc"].replace("Z","+00:00"))
            end=datetime.fromisoformat(case["run_returned_utc"].replace("Z","+00:00"))
            if end < start or case["manager_errors"]:
                raise ValueError("Incomplete run or manager errors")
            if any(not math.floor(start.timestamp()) <= int(case[k]) <= math.ceil(end.timestamp())
                   for k in ("dataset_timestamp","spectrum_timestamp")):
                raise ValueError("Stale result timestamp")
            if abs(D(p["System1/RoomTemp"]["data"])+D("273.15")-290)>D("1e-10"):
                raise ValueError("Wrong temperature")
            if abs(D(case["effective_device_temperature_c"][0])+D("273.15")-290)>D("1e-10"):
                raise ValueError("Wrong effective temperature")
            if abs(D(p["Sch1/PartList/Attn/ParamSet/L"]["data"])-1/g)>D("1e-14"):
                raise ValueError("Wrong actual loss")
            if m["CF"] != [1e8,1e8] or any(len(m[k])!=2 for k in ("CGAIN","CNF","CND","DCP")):
                raise ValueError("Unexpected measurement vector")
            if case["new_case"]:
                if datetime.fromisoformat(registered["registered_utc"]) >= start:
                    raise ValueError("Retrospective preregistration")
                if D(p["Sch1/PartList/Attn/ParamSet/L"]["data_entry"]) != loss:
                    raise ValueError("Wrong loss expression")
                if p["Sch1/PartList/Attn/ParamSet/Ta"]["data_entry"] != "" or p["Sch1/PartList/Source/ParamSet/Pwr"]["data_entry"] != "-160":
                    raise ValueError("Unexpected controlled parameters")
                if any(v.get("captured") is False for v in p.values()):
                    raise ValueError("Incomplete new parameter capture")
                for name, before in case["before"].items():
                    if any(before[f]!=case["restored"][name][f] for f in ("data","data_entry","evaluation_error")):
                        raise ValueError("Restoration mismatch")
                if case["collection_status"]["elapsed_seconds"] > 120 or case["collection_status"]["exit_code"] != 0:
                    raise ValueError("Unsuccessful bounded collection")
            checks={}
            endpoints=[]
            for frequency in (99999999.5,100000000.5):
                terms={t["role"]:t for t in case["spectral_terms"] if t["frequency_hz"]==frequency}
                if len(terms)!=4:
                    raise ValueError("Missing spectral roles")
                for role,t in terms.items():
                    if t["exported"]:
                        checks[str(frequency)+"_"+role+"_P_V_Z"]=checked(t["power"],voltage_power(t))
                total=terms["total_out"]; source=terms["source_out"];device=terms["device_out"]
                dev=D(device["power"]) if device["exported"] else D(0)
                dev50=voltage_power(device,50) if device["exported"] else D(0)
                checks[str(frequency)+"_raw_sum"]=checked(total["power"],D(source["power"])+dev)
                checks[str(frequency)+"_CND_voltage_50ohm"]=checked(m["CND"][1],voltage_power(total,50))
                checks[str(frequency)+"_source_signal_transport"]=checked(
                    voltage_power(source,50),K*290*D(m["DCP"][1])/D("1e-19"))
                checks[str(frequency)+"_budget_reconstruction"]=checked(
                    m["CND"][1],K*290*D(m["DCP"][1])/D("1e-19")+dev50)
                zr,zi=map(D,total["impedance"])
                endpoints.append(dict(frequency_hz=frequency,
                    path_to_raw_ratio=str(D(m["CND"][1])/D(total["power"])),
                    impedance_derived_ratio=str((zr*zr+zi*zi)/(50*zr)),
                    device_exported=device["exported"],device_noise_raw=str(dev) if device["exported"] else None,
                    device_noise_fraction_kT=str(dev/(K*290)) if device["exported"] else None,
                    ideal_device_noise_legacy=str(K*290*(1-g)),
                    device_ideal_relative_residual=relative(dev,K*290*(1-g)) if device["exported"] and loss>0 else None))
            checks["source_input_signal_noise_scale"]=checked(m["CND"][0],K*290*D(m["DCP"][0])/D("1e-19"))
            row=dict(loss_db=str(loss),new_case=case["new_case"],checks=checks,endpoints=endpoints,
                reported_gain=m["CGAIN"][1],reported_nf=m["CNF"][1],
                signal_transfer=str(D(m["DCP"][1])/D(m["DCP"][0])),
                signal_transfer_vs_ideal=relative(D(m["DCP"][1])/D(m["DCP"][0]),g),
                reported_gain_vs_transfer=relative(m["CGAIN"][1],D(m["DCP"][1])/D(m["DCP"][0])),
                binary64_gain_vs_50digit=relative(10**(-float(loss)/10),g),
                ideal_gain=str(g),ideal_nf=str(1/g))
            if case["new_case"]:
                pred=only([x for x in registered["cases"] if D(x["loss_db"])==loss])
                row["preregistered_hypothesis_results"]={}
                for name in ("hypothesis_round_to_unity_below_1e_6","hypothesis_no_threshold","hypothesis_db_threshold_1e_5"):
                    expected=pred[name]
                    row["preregistered_hypothesis_results"][name]=dict(
                        expected_unity=expected["CGAIN"]=="1" and expected["CNF"]=="1",
                        observed_unity=m["CGAIN"][1]==1 and m["CNF"][1]==1,
                        unity_pattern_matches=(expected["CGAIN"]=="1")== (m["CGAIN"][1]==1)
                                             and (expected["CNF"]=="1")== (m["CNF"][1]==1))
                row["preregistered_noise_prediction_residual"]=relative(m["CND"][1],pred["routine_path_prediction"])
            rows.append(row)
        new_rows = [r for r in rows if r["new_case"]]
        hypotheses = {
            name: all(r["preregistered_hypothesis_results"][name]["unity_pattern_matches"] for r in new_rows)
            for name in new_rows[0]["preregistered_hypothesis_results"]
        }
        unity_rows = [r for r in rows if r["reported_gain"] == 1 and r["reported_nf"] == 1]
        nonunity_rows = [r for r in rows if r["reported_gain"] != 1 and r["reported_nf"] != 1]
        bracket = [str(max(D(r["loss_db"]) for r in unity_rows)),
                   str(min(D(r["loss_db"]) for r in nonunity_rows))] if unity_rows and nonunity_rows else None
        return dict(schema_version=1,diagnostic_only=True,
            preregistered_hypotheses_consistent_with_two_new_unity_patterns=hypotheses,
            reconstruction_passed=all(x["passed"] for r in rows for x in r["checks"].values()),
            original_vendor_tolerance=1e-7,core_modified=False,new_analysis_count=2,cases=rows,
            interpretation={
                "hypothesis_scope":"Unity-pattern checks distinguish candidates without fitting thresholds; consistent does not mean uniquely established.",
                "conditional_transition_bracket_db":bracket,
                "bracket_condition":"Assumes monotonic single transition; upper bound is historical, not a new run.",
                "other_unexcluded_hypotheses":["Amplitude-domain tolerance","Other reporting floors or numerical branches"],
                "normalization":"All archived endpoints reconstruct CND as |Vtotal|^2/(2*50); raw P uses actual spectral Z. This is an algebraic observation, not proof of internal implementation.",
                "not_bypass":"Nonzero exported device noise and attenuated DCP persist while CGAIN/CNF equal unity.",
                "still_open":"Origin of near-zero effective transport/device-noise errors and exact reporting transition."})


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    sub=parser.add_subparsers(dest="mode",required=True)
    ex=sub.add_parser("extract");ex.add_argument("output",type=Path)
    replay=sub.add_parser("replay");replay.add_argument("evidence",type=Path);replay.add_argument("output",type=Path)
    args=parser.parse_args()
    if args.output.exists():
        raise ValueError("Refusing to overwrite existing artifact")
    result=extract() if args.mode=="extract" else analyze(load(args.evidence))
    args.output.write_text(json.dumps(result,indent=2,allow_nan=False)+"\n",encoding="utf-8")
    if args.mode=="replay":
        print(json.dumps({"reconstruction_passed":result["reconstruction_passed"],
                          "new_analysis_count":2,"vendor_acceptance":"see unchanged comparator report"}))
        return 0 if result["reconstruction_passed"] else 1
    return 0


if __name__=="__main__":
    raise SystemExit(main())

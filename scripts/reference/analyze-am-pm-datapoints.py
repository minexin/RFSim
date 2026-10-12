"""Offline hypotheses for DataPoints; not a vendor-compatible implementation."""

import argparse
import bisect
import cmath
import itertools
import json
import math
from pathlib import Path

AXES = ("dbm", "watt", "amplitude")
PHASE_RULES = ("numeric", "shortest", "phasor")
BOUNDARIES = ("constant", "linear")
COMPOSITIONS = ("add_source", "absolute")


def number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError("Expected a finite real number")
    value = float(value)
    if not math.isfinite(value):
        raise ValueError("Expected a finite real number")
    return value


def circular_error(actual, expected):
    return abs(math.remainder(number(actual) - number(expected), 360.0))


def validate_table(powers, phases):
    powers = [number(x) for x in powers]
    phases = [number(x) for x in phases]
    if len(powers) != len(phases) or len(powers) < 2:
        raise ValueError("Equal-length tables with at least two points required")
    if any(a >= b for a, b in zip(powers, powers[1:])):
        raise ValueError("Table powers must strictly increase")
    return powers, phases


def fraction(x, lo, hi, axis):
    if axis == "dbm":
        value = (x - lo) / (hi - lo)
    else:
        scale = math.log(10) / (10 if axis == "watt" else 20)
        try:
            value = math.expm1((x - lo) * scale) / math.expm1((hi - lo) * scale)
        except (OverflowError, ZeroDivisionError) as error:
            raise ValueError("Hypothesis axis is numerically unrepresentable") from error
    return number(value)


def predict(powers, phases, output_dbm, source_deg, axis, phase_rule, boundary, composition):
    powers, phases = validate_table(powers, phases)
    x, source = number(output_dbm), number(source_deg)
    if axis not in AXES or phase_rule not in PHASE_RULES:
        raise ValueError("Unknown axis or phase hypothesis")
    if boundary not in BOUNDARIES or composition not in COMPOSITIONS:
        raise ValueError("Unknown boundary or composition hypothesis")
    if boundary == "constant" and (x <= powers[0] or x >= powers[-1]):
        value = phases[0] if x <= powers[0] else phases[-1]
    else:
        i = min(max(bisect.bisect_right(powers, x) - 1, 0), len(powers) - 2)
        t = fraction(x, powers[i], powers[i + 1], axis)
        a, b = phases[i], phases[i + 1]
        if phase_rule == "numeric":
            value = a + t * (b - a)
        elif phase_rule == "shortest":
            delta = math.remainder(b - a, 360.0)
            if abs(delta) == 180:
                raise ValueError("Exactly antipodal shortest-angle hypothesis is ambiguous")
            value = a + t * delta
        else:
            z = (1 - t) * cmath.rect(1, math.radians(a)) + t * cmath.rect(1, math.radians(b))
            if abs(z) <= 1e-14:
                raise ValueError("Phasor interpolation has an undefined phase")
            value = math.degrees(cmath.phase(z))
    return number(value + (source if composition == "add_source" else 0))


def hypotheses():
    for values in itertools.product(AXES, PHASE_RULES, BOUNDARIES, COMPOSITIONS):
        yield dict(zip(("axis", "phase_rule", "boundary", "composition"), values))


def validate_plan(plan):
    tolerance = number(plan["diagnostic_phase_absolute_tolerance_degrees"])
    if tolerance <= 0:
        raise ValueError("Positive preregistered tolerance required")
    profiles = {}
    for profile in plan["profiles"]:
        name = profile["name"]
        if not isinstance(name, str) or not name or name in profiles:
            raise ValueError("Unique profile names required")
        validate_table(profile["power_dbm"], profile["phase_deg"])
        targets = [number(x) for x in profile["output_targets_dbm"]]
        if not targets or len(set(targets)) != len(targets):
            raise ValueError("Unique nonempty target list required")
        number(profile["source_phase_deg"])
        profiles[name] = profile
    if not profiles:
        raise ValueError("Empty plan")
    return profiles, tolerance


def analyze(plan, observations=None):
    profiles, tolerance = validate_plan(plan)
    rows = [] if observations is None else observations["samples"]
    seen, checked = set(), []
    for row in rows:
        name = row["profile"]
        if name not in profiles:
            raise ValueError("Observation outside preregistered profiles")
        profile = profiles[name]
        target = number(row["target_output_dbm"])
        if target not in profile["output_targets_dbm"] or (name, target) in seen:
            raise ValueError("Unexpected or duplicate observation target")
        seen.add((name, target))
        power = number(row["measured_output_w"])
        if power <= 0:
            raise ValueError("Measured output power must be positive")
        phase = number(row["measured_output_phase_deg"])
        source = number(row["measured_source_phase_deg"])
        if circular_error(source, profile["source_phase_deg"]) > tolerance:
            raise ValueError("Source phase readback differs from preregistered profile")
        checked.append((row, profile, 10 * math.log10(power) + 30, phase, source))
    results = []
    for hypothesis in hypotheses():
        residuals = []
        for row, profile, dbm, actual, source in checked:
            try:
                value = predict(
                    profile["power_dbm"], profile["phase_deg"], dbm, source, **hypothesis
                )
                residual = circular_error(actual, value)
                residuals.append(
                    dict(
                        profile=row["profile"],
                        target_output_dbm=row["target_output_dbm"],
                        actual_output_dbm=dbm,
                        predicted_phase_deg=value,
                        circular_error_deg=residual,
                        matched=residual <= tolerance,
                    )
                )
            except ValueError as error:
                residuals.append(dict(profile=row["profile"], matched=False, error=str(error)))
        results.append(
            dict(
                hypothesis=hypothesis,
                matched_all_observed=bool(residuals) and all(r["matched"] for r in residuals),
                residuals=residuals,
            )
        )
    planned = sum(len(p["output_targets_dbm"]) for p in profiles.values())
    return dict(
        classification="offline_hypothesis_comparison",
        eligible_for_vendor_compatibility=False,
        observation_provenance_verified=False,
        baseline_verified=False,
        limitations=[
            "Normalized observations require separate raw capture provenance verification",
            "Finite candidate list does not identify arbitrary spline or proprietary algorithms",
            "Endpoint sampling does not prove general extrapolation or derivative semantics",
        ],
        planned_sample_count=planned,
        observed_sample_count=len(checked),
        all_planned_samples_present=len(checked) == planned,
        phase_absolute_tolerance_degrees=tolerance,
        candidates=results,
    )


def natural_cubic(powers, phases, output_dbm):
    """Post-observation hypothesis: natural spline, raw angles, constant extension."""
    x, y = validate_table(powers, phases)
    q = number(output_dbm)
    if q <= x[0]:
        return y[0]
    if q >= x[-1]:
        return y[-1]
    n = len(x)
    h = [x[i + 1] - x[i] for i in range(n - 1)]
    slopes = [(y[i + 1] - y[i]) / h[i] for i in range(n - 1)]
    second = [0.0] * n
    diagonal, upper, rhs = [], [], []
    for i in range(1, n - 1):
        d = 2 * (h[i - 1] + h[i])
        r = 6 * (slopes[i] - slopes[i - 1])
        if diagonal:
            ratio = h[i - 1] / diagonal[-1]
            d -= ratio * upper[-1]
            r -= ratio * rhs[-1]
        diagonal.append(number(d))
        upper.append(h[i])
        rhs.append(number(r))
    for j in range(n - 3, -1, -1):
        second[j + 1] = (rhs[j] - upper[j] * second[j + 2]) / diagonal[j]
    i = min(bisect.bisect_right(x, q) - 1, n - 2)
    b = (q - x[i]) / h[i]
    a = 1 - b
    return number(
        a * y[i]
        + b * y[i + 1]
        + ((a**3 - a) * second[i] + (b**3 - b) * second[i + 1]) * h[i] ** 2 / 6
    )


def assess_natural_cubic(plan, observations, power_key="measured_output_w"):
    # Validate the primary observations even when displaying the separate raw-power diagnostic.
    analyze(plan, observations)
    profiles, tolerance = validate_plan(plan)
    rows = []
    for sample in observations["samples"]:
        profile = profiles[sample["profile"]]
        power = number(sample[power_key])
        if power <= 0:
            raise ValueError("Positive measured power required")
        dbm = 10 * math.log10(power) + 30
        predicted = natural_cubic(profile["power_dbm"], profile["phase_deg"], dbm)
        predicted += sample["measured_source_phase_deg"]
        residual = circular_error(sample["measured_output_phase_deg"], predicted)
        rows.append(
            dict(
                profile=sample["profile"],
                target_output_dbm=sample["target_output_dbm"],
                axis_output_dbm=dbm,
                predicted_phase_deg=predicted,
                observed_phase_deg=sample["measured_output_phase_deg"],
                circular_error_deg=residual,
                matched=residual <= tolerance,
            )
        )
    return dict(
        classification="post_observation_candidate_not_vendor_contract",
        power_axis_field=power_key,
        phase_absolute_tolerance_degrees=tolerance,
        all_observed_matched=bool(rows) and all(r["matched"] for r in rows),
        eligible_for_vendor_compatibility=False,
        rows=rows,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--observations", type=Path)
    args = parser.parse_args()
    plan = json.loads(args.plan.read_text(encoding="utf-8-sig"))
    observations = (
        None
        if args.observations is None
        else json.loads(args.observations.read_text(encoding="utf-8-sig"))
    )
    result = analyze(plan, observations)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2, allow_nan=False)
        stream.write("\n")


if __name__ == "__main__":
    main()

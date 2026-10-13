"""Data-only records for independent reference scorers, not a product binding.

These immutable tuples retain the reference scripts' field names and enum values.
They intentionally perform no RF calculations, native loading, or file dispatch.
"""
from enum import IntEnum
from typing import NamedTuple


class InterceptReference(IntEnum):
    INPUT = 0
    OUTPUT = 1


class TwoToneIntercept(NamedTuple):
    first_tone_order: int
    second_tone_order: int
    intercept_dbm: float
    coefficient_sign: int
    reference: InterceptReference = InterceptReference.INPUT


class SpectrumKind(IntEnum):
    SOURCE = 0
    HARMONIC = 1
    INTERMOD = 2


class CoherentComponent(NamedTuple):
    bin: int
    kind: SpectrumKind
    bandwidth_hz: float
    coherence_group: int
    amplitude: complex


class AmplifierOperatingPoint(NamedTuple):
    fundamental_amplitude_gain: float
    nonlinear_input_scale: float
    limited_input_power_w: float
    quadratic_voltage_coefficient: float
    cubic_voltage_coefficient: float


class CoherentPolynomialTerm(NamedTuple):
    order: int
    input_indices: tuple
    component: CoherentComponent


class CoherentPolynomialResponse(NamedTuple):
    inputs: tuple
    terms: tuple


class CoherentHighOrderAmplifierResponse(NamedTuple):
    inputs: tuple
    terms: tuple
    total_input_power_w: float
    operating_point: AmplifierOperatingPoint


class SourceCoherence(NamedTuple):
    source_id: str
    reference_clock: str = ""


class PortCoherentComponent(NamedTuple):
    input_port: int
    component: CoherentComponent

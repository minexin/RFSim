"""Multi-origin propagation for polynomial, mixer and linear-network graphs.

Physical coherence identities and stream-local source contributions are separate.
Temporary native groups enumerate contributions; they never determine output power.
"""

import cmath
import math
from dataclasses import dataclass

from . import CoherentComponent, CoherentMixerInput, OriginContribution, OriginFactor, SpectrumKind
from .coherence_file import _encode_reduction
from .coherent_network_file import _analyze_ports, _endpoint, _resolve_sources, _source_component
from .coherent_origins import OriginRegistry, component_key
from .coherent_graph_support import _array, _label
from .model_file import _number, _object


@dataclass(frozen=True)
class _Record:
    component: CoherentComponent
    identity: tuple[OriginFactor, ...]
    terms: tuple[OriginContribution, ...]


class _ExpressionGraph:
    def __init__(self, library, spacing, reference, groups, base_directory):
        self.library = library
        self.spacing = spacing
        self.reference = reference
        self.groups = groups
        self.base_directory = base_directory
        self.physical = OriginRegistry(library, max(groups.values(), default=0))
        self.history = OriginRegistry(library, 0)
        self.streams = {}
        self.reductions = {}
        self.mixed_groups = {}
        self.stored_components = 0
        self.stored_terms = 0
        self.stored_factors = 0

    def allocate(self):
        if self.physical.highest_group == 2**64 - 1:
            raise ValueError("Coherent group IDs exhausted")
        self.physical.highest_group += 1
        return self.physical.highest_group

    def read(self, name):
        name = _label(name)
        if name not in self.streams:
            raise ValueError("Unknown or forward-referenced stream: " + name)
        return self.streams[name]

    def new_ids(self, values):
        names = [_label(value) for value in values]
        if len(set(names)) != len(names) or any(name in self.streams for name in names):
            raise ValueError("Duplicate stream ID")
        if len(self.streams) + len(names) > 4096:
            raise ValueError("At most 4096 streams")
        return names

    def store(self, name, records):
        grouped = {}
        for record in records:
            component = self.physical.register(record.component, record.identity)
            key = component_key(component)
            if key not in grouped:
                grouped[key] = (component, record.identity, [])
            grouped[key][2].append(record.terms)
        combined = {}
        for key, (component, identity, parents) in grouped.items():
            expression = self.library.sum_origin_expressions(parents)
            for term in expression.terms:
                frequency = sum(
                    f.sign * self.history.roots[f.root_id - 1]["bin"] for f in term.factors
                )
                bandwidth = math.fsum(
                    self.history.roots[f.root_id - 1]["bandwidth_hz"] for f in term.factors
                )
                if frequency != component.bin or not math.isclose(
                    bandwidth, component.bandwidth_hz, rel_tol=1e-12, abs_tol=0.0
                ):
                    raise ValueError("Source contribution disagrees with frequency/bandwidth")
            component = component._replace(amplitude=expression.amplitude)
            combined[key] = _Record(component, identity, expression.terms)
            self.stored_components += 1
            self.stored_terms += len(expression.terms)
            self.stored_factors += sum(len(term.factors) for term in expression.terms)
            if (
                self.stored_components > 65536
                or self.stored_terms > 65536
                or self.stored_factors > 1048576
            ):
                raise ValueError("Stored graph contribution limit exceeded")
        reduction = self.library.reduce_coherent_components(
            self.spacing, [record.component for record in combined.values()]
        )
        self.reductions[name] = reduction
        self.streams[name] = tuple(combined[component_key(c)] for c in reduction.components)

    def flatten(self, records):
        """Give each actual contribution a temporary ID for native linear algebra."""
        components, lookup = [], {}
        for record in records:
            for term in record.terms:
                component = record.component._replace(
                    coherence_group=self.allocate(), amplitude=term.amplitude
                )
                components.append(component)
                lookup[component.coherence_group] = (record.identity, term.factors)
                if len(components) > 4096:
                    raise ValueError("At most 4096 expanded input contributions")
        return components, lookup

    def encode(self, record):
        component = record.component
        return {
            "bin": component.bin,
            "kind": component.kind.name.lower(),
            "bandwidth_hz": component.bandwidth_hz,
            "coherence_group": component.coherence_group,
            "terms": [
                {
                    "source_order": len(term.factors),
                    "rf_order": sum(
                        self.history.roots[f.root_id - 1]["role"] == "rf" for f in term.factors
                    ),
                    "source_factors": [
                        {"root_id": f.root_id, "sign": f.sign} for f in term.factors
                    ],
                    "amplitude": [term.amplitude.real, term.amplitude.imag],
                }
                for term in record.terms
            ],
        }

    def generate(self, parents, coefficients, maximum=None):
        """Expand nonlinear contributions using a previously solved common response."""
        expanded, lookup = self.flatten(parents)
        response = self.library.coherent_polynomial(
            self.spacing,
            expanded,
            coefficients,
            reference_ohms=self.reference,
            reserved_group_max=self.physical.highest_group,
        )
        self.physical.highest_group = max(
            [self.physical.highest_group] + [t.component.coherence_group for t in response.terms]
        )
        identities = [lookup[c.coherence_group][0] for c in response.inputs]
        origins = [lookup[c.coherence_group][1] for c in response.inputs]
        output, discarded = [], {}
        for term in response.terms:
            order = sum(len(origins[abs(i) - 1]) for i in term.input_indices)
            if maximum is not None and order > maximum:
                discarded[order] = discarded.get(order, 0) + 1
                continue
            identity = self.physical.compose(identities, term.input_indices)
            origin = self.history.compose(origins, term.input_indices)
            component = term.component
            if term.order > 1:
                harmonic = all(f.sign == 1 and f.root_id == identity[0].root_id for f in identity)
                component = component._replace(
                    kind=SpectrumKind.HARMONIC if harmonic else SpectrumKind.INTERMOD
                )
            output.append(
                _Record(component, identity, (OriginContribution(origin, component.amplitude),))
            )
        return output, {
            "expanded_input_count": len(expanded),
            "physical_input_count": len(parents),
            "generated_term_count": len(response.terms),
            "discarded_term_count": sum(discarded.values()),
            "discarded_by_source_order": [
                {"source_order": order, "term_count": count}
                for order, count in sorted(discarded.items())
            ],
        }

    def polynomial(self, stage):
        _object(
            stage, ("id", "type", "input", "output", "voltage_coefficients", "max_source_order")
        )
        maximum = stage["max_source_order"]
        if type(maximum) is not int or not 1 <= maximum <= 256:
            raise ValueError("max_source_order must be an integer from 1 to 256")
        coefficients = [
            _number(v) for v in _array(stage["voltage_coefficients"], 12, nonempty=True)
        ]
        names = self.new_ids([stage["output"]])
        output, measurements = self.generate(self.read(stage["input"]), coefficients, maximum)
        self.store(names[0], output)
        return names, dict(measurements, max_source_order=maximum)

    def scaled(self, records, gain):
        """Scale every source contribution without dividing by its aggregate wave."""
        output = []
        for record in records:
            expression = self.library.product_origin_expressions(
                [record.terms], [1], coefficient=gain
            )
            output.append(
                _Record(
                    record.component._replace(amplitude=expression.amplitude),
                    record.identity,
                    expression.terms,
                )
            )
        return output

    def amplifier(self, stage):
        common = ("power_gain_db", "output_p1db_dbm", "output_saturation_dbm")
        fundamental_only = stage["type"] == "fundamental_compression"
        parameters = common if fundamental_only else common + ("input_ip2_dbm", "input_ip3_dbm")
        _object(stage, ("id", "type", "input", "output") + parameters)
        values = {key: _number(stage[key]) for key in parameters}
        names = self.new_ids([stage["output"]])
        parents = self.read(stage["input"])
        cascade = stage["type"] == "cascaded_amplifier"
        if not cascade and any(r.component.kind != SpectrumKind.SOURCE for r in parents):
            raise ValueError("Fundamental/limited amplification requires source-kind inputs")
        # Drive is computed from real coherent components, never flattened origins.
        drive = self.reductions[stage["input"]].total_power_w
        if fundamental_only:
            gain = self.library.saturating_amplitude_gain(drive, **values)
            output = self.scaled(parents, gain)
            measurements = {"input_power_w": drive, "fundamental_amplitude_gain": gain}
        else:
            point = self.library.amplifier_operating_point(
                drive, reference_ohms=self.reference, **values
            )
            output = self.scaled(parents, point.fundamental_amplitude_gain)
            carriers = [r for r in parents if r.component.kind == SpectrumKind.SOURCE]
            nonlinear, measurements = self.generate(
                self.scaled(carriers, point.nonlinear_input_scale),
                [0.0, 0.0, point.quadratic_voltage_coefficient, point.cubic_voltage_coefficient],
            )
            output.extend(nonlinear)
            measurements.update(
                input_power_w=drive,
                limited_input_power_w=point.limited_input_power_w,
                fundamental_amplitude_gain=point.fundamental_amplitude_gain,
                nonlinear_input_scale=point.nonlinear_input_scale,
            )
        self.store(names[0], output)
        return names, measurements

    def mixer(self, stage):
        _object(stage, ("id", "type", "branches"))
        branches = _array(stage["branches"], 2048, nonempty=True)
        for branch in branches:
            _object(
                branch,
                ("id", "input", "lo_source", "lo_bin", "conversion_gain_db"),
                ("lo_phase_radians",),
            )
        names = self.new_ids([branch["id"] for branch in branches])
        incident, metadata, sizes = [], [], []
        for branch in branches:
            source = _label(branch["lo_source"])
            if source not in self.groups:
                raise ValueError("Unknown LO source")
            gain = _number(branch["conversion_gain_db"])
            phase = _number(branch.get("lo_phase_radians", 0.0))
            self.library.ideal_mixer(
                self.spacing,
                {},
                lo_bin=branch["lo_bin"],
                conversion_gain_db=gain,
                lo_phase_radians=phase,
                reference_ohms=self.reference,
            )
            records = self.read(branch["input"])
            sizes.append(len(records))
            lo = self.history.root(self.groups[source], branch["lo_bin"], 0.0, "lo")
            for record in records:
                incident.append(
                    CoherentMixerInput(
                        record.component, branch["lo_bin"], gain, phase, self.groups[source]
                    )
                )
                metadata.append((record, lo))
                if len(incident) > 2048:
                    raise ValueError("Mixer bank accepts at most 2048 RF components")
        converted = self.library.mix_coherent_components(
            self.spacing, incident, reserved_group_max=self.physical.highest_group
        )
        self.physical.highest_group = max(
            [self.physical.highest_group] + [c.coherence_group for c in converted]
        )
        output = []
        for position, (value, (record, lo)) in enumerate(zip(incident, metadata)):
            key = (value.component.coherence_group, value.lo_coherence_group)
            shared = self.mixed_groups.setdefault(key, converted[2 * position].coherence_group)
            pump = (OriginContribution(lo, cmath.exp(1j * value.lo_phase_radians)),)
            difference = (1, -2) if value.component.bin > value.lo_bin else (-1, 2)
            for side, indices in enumerate((difference, (1, 2))):
                component = converted[2 * position + side]._replace(coherence_group=shared)
                expression = self.library.product_origin_expressions(
                    [record.terms, pump],
                    indices,
                    coefficient=10.0 ** (value.conversion_gain_db / 20.0),
                )
                identity = self.physical.root(
                    shared, component.bin, component.bandwidth_hz, "mixed"
                )
                output.append(_Record(component, identity, expression.terms))
        offset = 0
        for name, count in zip(names, sizes):
            stop = offset + 2 * count
            self.store(name, output[offset:stop])
            offset = stop
        return names, {}

    def linear(self, stage):
        _object(stage, ("id", "type", "network", "inputs", "outputs"))
        values = _array(stage["outputs"], 1024, nonempty=True)
        for value in values:
            _object(value, ("id", "port"))
        names = self.new_ids([value["id"] for value in values])
        ports = [_endpoint(value["port"]) for value in values]
        incident, declared, lookup = [], [], {}
        for value in _array(stage["inputs"], 4096):
            _object(value, ("stream", "port"))
            port = _endpoint(value["port"])
            declared.append(port)
            components, origins = self.flatten(self.read(value["stream"]))
            lookup.update(origins)
            incident.extend((port, c) for c in components)
            if len(incident) > 4096:
                raise ValueError("At most 4096 expanded incident port contributions")
        results = _analyze_ports(
            self.library,
            self.spacing,
            self.reference,
            stage["network"],
            incident,
            ports,
            self.base_directory,
            declared_inputs=declared,
        )
        for name, port in zip(names, ports):
            records = []
            for component in results[port].components:
                identity, origin = lookup[component.coherence_group]
                records.append(
                    _Record(component, identity, (OriginContribution(origin, component.amplitude),))
                )
            self.store(name, records)
        return names, {}


def analyze_expression_system(library, document, *, base_directory=None):
    """Execute the already format-validated mixed polynomial graph."""
    stages = _array(document["stages"], 512)
    supported = {
        "linear_network",
        "polynomial_amplifier",
        "ideal_mixer_bank",
        "fundamental_compression",
        "limited_amplifier",
        "cascaded_amplifier",
    }
    if any(not isinstance(stage, dict) or stage.get("type") not in supported for stage in stages):
        raise ValueError("Unsupported stage in multi-origin coherent graph")
    groups, sources = _resolve_sources(library, document["sources"])
    spacing = _number(document["spacing_hz"])
    reference = _number(document.get("reference_ohms", 50.0))
    graph = _ExpressionGraph(library, spacing, reference, groups, base_directory)
    inputs = _array(document["inputs"], 4096)
    graph.new_ids([v.get("id") if isinstance(v, dict) else None for v in inputs])
    for value in inputs:
        _object(value, ("id", "components"))
        reduced = library.reduce_coherent_components(
            spacing, [_source_component(c, groups) for c in _array(value["components"], 4096)]
        )
        records = []
        for component in reduced.components:
            identity = graph.physical.root(
                component.coherence_group, component.bin, component.bandwidth_hz, "rf"
            )
            origin = graph.history.root(
                component.coherence_group, component.bin, component.bandwidth_hz, "rf"
            )
            records.append(
                _Record(component, identity, (OriginContribution(origin, component.amplitude),))
            )
        graph.store(value["id"], records)
    identifiers, results = set(), []
    dispatch = {
        "linear_network": graph.linear,
        "polynomial_amplifier": graph.polynomial,
        "ideal_mixer_bank": graph.mixer,
        "fundamental_compression": graph.amplifier,
        "limited_amplifier": graph.amplifier,
        "cascaded_amplifier": graph.amplifier,
    }
    for stage in stages:
        name = _label(stage.get("id"))
        if name in identifiers:
            raise ValueError("Duplicate stage ID")
        identifiers.add(name)
        names, measurements = dispatch[stage["type"]](stage)
        results.append({"id": name, "type": stage["type"], "outputs": names, **measurements})
    outputs = [_label(name) for name in _array(document["outputs"], 4096, nonempty=True)]
    if len(set(outputs)) != len(outputs):
        raise ValueError("Duplicate requested output")
    for name in outputs:
        graph.read(name)
    return {
        "format": "rfmodel.coherent-system-result",
        "version": 1,
        "origin_representation": "complex-expression",
        "source_order_policy": "rf-and-lo-factors",
        "spacing_hz": spacing,
        "reference_ohms": reference,
        "sources": sources,
        "origin_roots": graph.history.roots,
        "stages": results,
        "outputs": outputs,
        "streams": [
            {
                "id": name,
                **_encode_reduction(graph.reductions[name], spacing),
                "origins": [graph.encode(record) for record in records],
            }
            for name, records in graph.streams.items()
        ],
    }

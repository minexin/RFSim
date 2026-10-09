"""Analysis-local source monomials for coherent feed-forward graphs."""

import math

from . import OriginFactor


def component_key(component):
    return (component.bin, component.kind, component.bandwidth_hz, component.coherence_group)


class OriginRegistry:
    def __init__(self, library, reserved_group_max):
        self.library = library
        self.highest_group = reserved_group_max
        self.roots = []
        self.root_ids = {}
        self.origins = {}
        self.groups = {}
        self.stored_factors = 0

    def root(self, group, bin_index, bandwidth, role):
        key = (group, bin_index, bandwidth, role)
        if key not in self.root_ids:
            if len(self.roots) >= 65536:
                raise ValueError("Origin registry exceeds 65536 roots")
            root_id = len(self.roots) + 1
            self.root_ids[key] = root_id
            self.roots.append(
                {
                    "root_id": root_id,
                    "coherence_group": group,
                    "bin": bin_index,
                    "bandwidth_hz": bandwidth,
                    "role": role,
                }
            )
        return (OriginFactor(self.root_ids[key], 1),)

    def seed(self, component):
        origin = self.root(component.coherence_group, component.bin, component.bandwidth_hz, "rf")
        return self.register(component, origin)

    def lookup(self, component):
        try:
            return self.origins[component_key(component)]
        except KeyError as error:
            raise ValueError("Missing origin for a propagated component") from error

    def compose(self, parents, indices):
        # Only the selected parents need native encoding, at most nine per product.
        selected = [parents[abs(index) - 1] for index in indices]
        local = [
            position + 1 if index > 0 else -(position + 1) for position, index in enumerate(indices)
        ]
        return self.library.expand_mixing_origin(selected, local)

    def register(self, component, origin):
        frequency = sum(factor.sign * self.roots[factor.root_id - 1]["bin"] for factor in origin)
        bandwidth = math.fsum(self.roots[factor.root_id - 1]["bandwidth_hz"] for factor in origin)
        if frequency != component.bin or not math.isclose(
            bandwidth, component.bandwidth_hz, rel_tol=1e-12, abs_tol=0.0
        ):
            raise ValueError("Expanded origin disagrees with frequency/bandwidth")
        # Equal monomials must share the same support width even if local
        # parenthesization accumulated the input bandwidths in a different order.
        component = component._replace(bandwidth_hz=bandwidth)
        self.highest_group = max(self.highest_group, component.coherence_group)
        canonical = (component.bin, component.kind, component.bandwidth_hz, origin)
        if canonical in self.groups:
            component = component._replace(coherence_group=self.groups[canonical])
        else:
            previous = self.origins.get(component_key(component))
            if previous is not None and previous != origin:
                raise ValueError("A coherent component contains distinct source expressions")
            self.groups[canonical] = component.coherence_group
        key = component_key(component)
        if key not in self.origins:
            if self.stored_factors + len(origin) > 1048576:
                raise ValueError("Origin registry exceeds 1048576 stored factors")
            self.stored_factors += len(origin)
            self.origins[key] = origin
        elif self.origins[key] != origin:
            raise ValueError("Conflicting coherent origin identity")
        return component

    def encode(self, component):
        origin = self.lookup(component)
        return {
            "bin": component.bin,
            "kind": component.kind.name.lower(),
            "bandwidth_hz": component.bandwidth_hz,
            "coherence_group": component.coherence_group,
            "source_order": len(origin),
            "source_factors": [
                {"root_id": factor.root_id, "sign": factor.sign} for factor in origin
            ],
        }

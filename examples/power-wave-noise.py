"""Usage: python power-wave-noise.py /path/to/rfmodel_c shared library."""
import json
import math
import sys

from rfmodel import Library, NoiseParameters

library = Library(sys.argv[1])
scattering = [[0.2 - 0.1j, 0.03], [2 + 1j, -0.15]]
original = NoiseParameters(10 * math.log10(2), 0.2 + 0.3j, 57.375)
noise = library.noise_from_power_wave_parameters(scattering, original, [50, 50])
references = [25 + 10j, 100 - 15j]
converted = library.renormalize_power_waves(scattering, [50, 50], references, noise=noise)
parameters = library.power_wave_noise_parameters(
    converted.scattering, converted.noise_correlation, references
)
result = {
    "reference_impedances_ohms": [[z.real, z.imag] for z in references],
    "minimum_noise_figure_db": parameters.minimum_noise_figure_db,
    "optimum_source_reflection": [
        parameters.optimum_source_reflection.real,
        parameters.optimum_source_reflection.imag,
    ],
    "noise_resistance_ohms": parameters.noise_resistance_ohms,
    "source_impedance_ohms": [25, 30],
    "noise_figure_db": library.power_wave_noise_figure(
        converted.scattering, converted.noise_correlation, references, 25 + 30j
    ),
}
print(json.dumps(result, indent=2, allow_nan=False))

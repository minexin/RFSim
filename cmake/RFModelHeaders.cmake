# Canonical public header inventory. Add new headers to their owning module here.
# Flat include compatibility is generated in the build tree, not duplicated in source.
set(RFMODEL_PUBLIC_HEADERS
    adapters/model_adapter.hpp
    adapters/builtin_model_adapters.hpp
    adapters/systemvue2023_am_pm.hpp
    analysis/analysis_task.hpp
    analysis/group_delay.hpp
    analysis/measurements.hpp
    analysis/monte_carlo.hpp
    analysis/optimization.hpp
    analysis/power_gain.hpp
    analysis/spectrum_analysis.hpp
    api/c_api.h
    coherent/coherence.hpp
    coherent/coherent_amplifier.hpp
    coherent/coherent_compression.hpp
    coherent/coherent_highorder_amplifier.hpp
    coherent/coherent_mixer.hpp
    coherent/coherent_network.hpp
    coherent/coherent_pipeline.hpp
    coherent/coherent_polynomial.hpp
    coherent/coherent_system.hpp
    coherent/mixing_origin.hpp
    coherent/origin_expression.hpp
    coherent/source_coherence.hpp
    coherent/term_propagation.hpp
    conversion/am_pm_linearization.hpp
    conversion/amplifier_linearization.hpp
    conversion/tabulated_amplifier_linearization.hpp
    conversion/conversion_frequency_plan.hpp
    conversion/conversion_grid.hpp
    conversion/conversion_network.hpp
    conversion/conversion_operating_point.hpp
    conversion/conversion_power_wave.hpp
    conversion/conversion_simulation.hpp
    conversion/frequency_conversion.hpp
    conversion/highorder_linearization.hpp
    conversion/mixer_linearization.hpp
    conversion/polynomial_linearization.hpp
    conversion/output_phase_linearization.hpp
    core/device_model.hpp
    core/interpolation.hpp
    core/linear_solver.hpp
    core/network_parameters.hpp
    core/power_wave_reference.hpp
    io/tabulated_model.hpp
    io/tabulated_noise_model.hpp
    io/touchstone.hpp
    io/touchstone_writer.hpp
    linear/linear_analysis.hpp
    linear/linear_path.hpp
    linear/network.hpp
    linear/power_wave_network.hpp
    linear/sweep.hpp
    models/butterworth_filter.hpp
    models/chebyshev_filter.hpp
    models/filter_frequency.hpp
    models/frequency_dependent_cable.hpp
    models/ideal_devices.hpp
    models/lumped_model.hpp
    models/multiport_devices.hpp
    models/rlc_model.hpp
    models/rlgc_transmission_line.hpp
    models/transmission_line.hpp
    noise/channel_noise.hpp
    noise/linear_path_noise.hpp
    noise/loaded_noise.hpp
    noise/noise.hpp
    noise/noise_figure.hpp
    noise/noise_matrix.hpp
    noise/noise_renormalization.hpp
    noise/phase_noise.hpp
    noise/power_wave_noise.hpp
    nonlinear/phd_coefficient_slice.hpp
    nonlinear/phd_fixed_pump_network.hpp
    nonlinear/am_pm.hpp
    nonlinear/amplifier_model.hpp
    nonlinear/tabulated_amplifier.hpp
    nonlinear/fundamental_compression.hpp
    nonlinear/ideal_mixer.hpp
    nonlinear/intermod_levels.hpp
    nonlinear/multitone_amplifier.hpp
    nonlinear/nonlinear_spectrum.hpp
    nonlinear/polynomial_amplifier.hpp
    nonlinear/point_derivative_am_pm.hpp
    nonlinear/polynomial_intercepts.hpp
    nonlinear/polynomial_limits.hpp
    nonlinear/power_wave_spectrum.hpp
    nonlinear/saturating_fundamental.hpp
    nonlinear/single_tone_amplifier.hpp
)
file(GLOB_RECURSE actual_headers CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include/rfmodel"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/rfmodel/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/rfmodel/*.h")
list(SORT actual_headers)
set(declared_headers ${RFMODEL_PUBLIC_HEADERS})
list(SORT declared_headers)
if(NOT actual_headers STREQUAL declared_headers)
    message(FATAL_ERROR "Public header inventory differs from module layout; update cmake/RFModelHeaders.cmake")
endif()
set(RFMODEL_LEGACY_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/legacy-include")
file(MAKE_DIRECTORY "${RFMODEL_LEGACY_INCLUDE_DIR}/rfmodel")
set(legacy_names)
foreach(header IN LISTS RFMODEL_PUBLIC_HEADERS)
    get_filename_component(legacy_name "${header}" NAME)
    if(legacy_name IN_LIST legacy_names)
        message(FATAL_ERROR "Public header basename collision: ${legacy_name}")
    endif()
    list(APPEND legacy_names "${legacy_name}")
    file(CONFIGURE OUTPUT "${RFMODEL_LEGACY_INCLUDE_DIR}/rfmodel/${legacy_name}"
        CONTENT "#pragma once\n// Compatibility include. Prefer the module-qualified path in new code.\n#include <rfmodel/${header}>\n"
        @ONLY NEWLINE_STYLE LF)
endforeach()

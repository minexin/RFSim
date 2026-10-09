cmake_minimum_required(VERSION 3.20)
project(RFModel VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_library(rfmodel INTERFACE)
add_library(RFModel::rfmodel ALIAS rfmodel)
target_compile_features(rfmodel INTERFACE cxx_std_17)
target_include_directories(rfmodel INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
include(GNUInstallDirs)
option(RFMODEL_BUILD_C_API "Build the shared C ABI for linear networks" ON)
if(RFMODEL_BUILD_C_API)
    enable_language(C)
    add_library(rfmodel_c SHARED src/c_api.cpp)
    add_library(RFModel::c_api ALIAS rfmodel_c)
    set_target_properties(rfmodel_c PROPERTIES EXPORT_NAME c_api CXX_VISIBILITY_PRESET hidden
        DEBUG_POSTFIX d)
    target_compile_definitions(rfmodel_c PRIVATE RFMODEL_C_API_BUILD)
    target_link_libraries(rfmodel_c PUBLIC RFModel::rfmodel)
    install(TARGETS rfmodel_c EXPORT RFModelTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
include(CMakePackageConfigHelpers)
install(TARGETS rfmodel EXPORT RFModelTargets)
install(DIRECTORY include/rfmodel DESTINATION include)
install(EXPORT RFModelTargets NAMESPACE RFModel:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RFModel)
configure_package_config_file(cmake/RFModelConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/RFModelConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RFModel)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/RFModelConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/RFModelConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/RFModelConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RFModel)
include(CTest)
option(RFMODEL_BUILD_CLI "Build the batch S parameter CSV exporter" ON)
if(RFMODEL_BUILD_CLI)
add_executable(rfmodel_cli src/rfmodel_cli.cpp)
target_link_libraries(rfmodel_cli PRIVATE RFModel::rfmodel)
install(TARGETS rfmodel_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(BUILD_TESTING)
if(RFMODEL_BUILD_C_API)
    add_executable(c_api_test tests/c_api_test.c)
    target_link_libraries(c_api_test PRIVATE RFModel::c_api)
    if(UNIX)
        # C callers using math functions must link libm explicitly on Unix.
        target_link_libraries(c_api_test PRIVATE m)
    endif()
    add_test(NAME c_api_test COMMAND c_api_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/sweep.s2p"
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/noisy-amplifier.s2p")
    option(RFMODEL_REQUIRE_PYTHON_TESTS "Require Python binding tests during configuration" OFF)
    if(RFMODEL_REQUIRE_PYTHON_TESTS)
        find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
    else()
        find_package(Python3 3.9 COMPONENTS Interpreter QUIET)
    endif()
    if(Python3_Interpreter_FOUND)
        add_test(NAME python_api_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python_api.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME linear_noise_analysis_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_linear_noise_analysis.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME amplifier_operating_point_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_amplifier_operating_point.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME conversion_operating_point_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_operating_point.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME affine_conversion_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_affine_conversion.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME mixer_linearization_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mixer_linearization.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME shared_phase_noise_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_shared_phase_noise.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME phase_noise_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_phase_noise.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME channel_noise_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_channel_noise.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME conversion_loaded_noise_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_conversion_loaded_noise.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME conversion_noise_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_conversion_noise.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME conversion_network_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_conversion_network.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME frequency_conversion_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_frequency_conversion.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME chebyshev_filter_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_chebyshev_filter.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME butterworth_filter_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_butterworth_filter.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME passive_model_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_passive_model_file.py $<TARGET_FILE:rfmodel_c>)
        add_test(NAME native_compression_reference COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_compare_antenna_compression.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME single_compression_reference COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_compare_single_compression.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME single_harmonic_reference COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_compare_single_harmonics.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME saturation_reference_diagnostic COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_compare_saturating_fundamental.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME limited_single_tone_reference COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_compare_limited_single_tone.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME two_tone_reference COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_compare_two_tone.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME multitone_amplifier_reference COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_compare_multitone_amplifier.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME amplifier_term_reference COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_compare_amplifier_terms.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME coherent_system_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_coherent_system_file.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME coherent_network_file_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_coherent_network_file.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME highorder_model_reference COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_highorder_model_reference.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME highorder_input_reference_diagnostic COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_highorder_input_reference.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME eleventh_order_diagnostic COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_diagnose_eleventh_order.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME spectrum_reduction_diagnostic COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_diagnose_spectrum_reduction.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME highorder_amplifier_diagnostic COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_diagnose_highorder_amplifier.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME cascade_unity_gain_diagnostic COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_diagnose_cascade_unity_gain.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME cascade_amplifier_reference COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_compare_cascade_amplifier.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME coherent_amplifier_reference COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_compare_coherent_amplifier.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME shared_compression_reference COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_compare_shared_compression.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME coherent_network_reference COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_compare_coherent_network.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME coherent_carrier_reference COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_compare_coherent_carriers.py
            $<TARGET_FILE:rfmodel_c>)
        add_test(NAME total_drive_diagnostic COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_diagnose_total_drive.py
            $<TARGET_FILE:rfmodel_c>)
    endif()
endif()
if(RFMODEL_BUILD_CLI)
add_test(NAME cli_export COMMAND ${CMAKE_COMMAND}
    -DCLI=$<TARGET_FILE:rfmodel_cli> -DFIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/tests
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli.cmake)
endif()
add_executable(rfmodel_smoke tests/smoke.cpp)
target_link_libraries(rfmodel_smoke PRIVATE rfmodel)
add_executable(linear_solver_smoke tests/linear_solver_smoke.cpp)
target_link_libraries(linear_solver_smoke PRIVATE rfmodel)
add_test(NAME rfmodel_smoke COMMAND rfmodel_smoke)
add_test(NAME linear_solver_smoke COMMAND linear_solver_smoke)
add_executable(touchstone_smoke tests/touchstone_smoke.cpp)
target_link_libraries(touchstone_smoke PRIVATE rfmodel)
add_test(NAME touchstone_smoke COMMAND touchstone_smoke "${CMAKE_CURRENT_SOURCE_DIR}/tests")

add_executable(touchstone_validation tests/touchstone_validation.cpp)
target_link_libraries(touchstone_validation PRIVATE rfmodel)
add_test(NAME touchstone_validation COMMAND touchstone_validation "${CMAKE_CURRENT_SOURCE_DIR}/tests")
add_executable(network_test tests/network_test.cpp)
target_link_libraries(network_test PRIVATE rfmodel)
add_test(NAME network_test COMMAND network_test)
add_executable(sweep_test tests/sweep_test.cpp)
target_link_libraries(sweep_test PRIVATE rfmodel)
add_test(NAME sweep_test COMMAND sweep_test "${CMAKE_CURRENT_SOURCE_DIR}/tests")
add_executable(measurements_test tests/measurements_test.cpp)
target_link_libraries(measurements_test PRIVATE rfmodel)
add_test(NAME measurements_test COMMAND measurements_test)
add_executable(noise_test tests/noise_test.cpp)
target_link_libraries(noise_test PRIVATE rfmodel)
add_test(NAME noise_test COMMAND noise_test)
add_executable(external_s_test tests/external_s_test.cpp)
target_link_libraries(external_s_test PRIVATE rfmodel)
add_test(NAME external_s_test COMMAND external_s_test)
add_executable(linear_analysis_test tests/linear_analysis_test.cpp)
target_link_libraries(linear_analysis_test PRIVATE rfmodel)
add_test(NAME linear_analysis_test COMMAND linear_analysis_test)
add_executable(tabulated_model_test tests/tabulated_model_test.cpp)
target_link_libraries(tabulated_model_test PRIVATE rfmodel)
add_test(NAME tabulated_model_test COMMAND tabulated_model_test "${CMAKE_CURRENT_SOURCE_DIR}/tests")
add_executable(ideal_devices_test tests/ideal_devices_test.cpp)
target_link_libraries(ideal_devices_test PRIVATE rfmodel)
add_test(NAME ideal_devices_test COMMAND ideal_devices_test)
add_executable(transmission_line_test tests/transmission_line_test.cpp)
target_link_libraries(transmission_line_test PRIVATE rfmodel)
add_test(NAME transmission_line_test COMMAND transmission_line_test)
add_executable(rlgc_transmission_line_test tests/rlgc_transmission_line_test.cpp)
target_link_libraries(rlgc_transmission_line_test PRIVATE rfmodel)
add_test(NAME rlgc_transmission_line_test COMMAND rlgc_transmission_line_test)
add_executable(multiport_devices_test tests/multiport_devices_test.cpp)
target_link_libraries(multiport_devices_test PRIVATE rfmodel)
add_test(NAME multiport_devices_test COMMAND multiport_devices_test)
add_executable(impedance_test tests/impedance_test.cpp)
target_link_libraries(impedance_test PRIVATE rfmodel)
add_test(NAME impedance_test COMMAND impedance_test)
add_executable(group_delay_test tests/group_delay_test.cpp)
target_link_libraries(group_delay_test PRIVATE rfmodel)
add_test(NAME group_delay_test COMMAND group_delay_test)
add_executable(network_parameters_test tests/network_parameters_test.cpp)
target_link_libraries(network_parameters_test PRIVATE rfmodel)
add_test(NAME network_parameters_test COMMAND network_parameters_test)
add_executable(renormalization_test tests/renormalization_test.cpp)
target_link_libraries(renormalization_test PRIVATE rfmodel)
add_test(NAME renormalization_test COMMAND renormalization_test)
add_executable(lumped_model_test tests/lumped_model_test.cpp)
target_link_libraries(lumped_model_test PRIVATE rfmodel)
add_test(NAME lumped_model_test COMMAND lumped_model_test)
add_executable(rlc_model_test tests/rlc_model_test.cpp)
target_link_libraries(rlc_model_test PRIVATE rfmodel)
add_test(NAME rlc_model_test COMMAND rlc_model_test)
add_executable(noise_matrix_test tests/noise_matrix_test.cpp)
target_link_libraries(noise_matrix_test PRIVATE rfmodel)
add_test(NAME noise_matrix_test COMMAND noise_matrix_test)
add_executable(noise_renormalization_test tests/noise_renormalization_test.cpp)
target_link_libraries(noise_renormalization_test PRIVATE rfmodel)
add_test(NAME noise_renormalization_test COMMAND noise_renormalization_test)
add_executable(network_noise_test tests/network_noise_test.cpp)
target_link_libraries(network_noise_test PRIVATE rfmodel)
add_test(NAME network_noise_test COMMAND network_noise_test)
add_executable(linear_noise_test tests/linear_noise_test.cpp)
target_link_libraries(linear_noise_test PRIVATE rfmodel)
add_test(NAME linear_noise_test COMMAND linear_noise_test)
add_executable(noise_figure_test tests/noise_figure_test.cpp)
target_link_libraries(noise_figure_test PRIVATE rfmodel)
add_test(NAME noise_figure_test COMMAND noise_figure_test)
add_executable(noise_parameters_test tests/noise_parameters_test.cpp)
target_link_libraries(noise_parameters_test PRIVATE rfmodel)
add_test(NAME noise_parameters_test COMMAND noise_parameters_test)
add_executable(noise_import_test tests/noise_import_test.cpp)
target_link_libraries(noise_import_test PRIVATE rfmodel)
add_test(NAME noise_import_test COMMAND noise_import_test)
add_executable(tabulated_noise_test tests/tabulated_noise_test.cpp)
target_link_libraries(tabulated_noise_test PRIVATE rfmodel)
add_test(NAME tabulated_noise_test COMMAND tabulated_noise_test)
add_executable(touchstone_noise_test tests/touchstone_noise_test.cpp)
target_link_libraries(touchstone_noise_test PRIVATE rfmodel)
add_test(NAME touchstone_noise_test COMMAND touchstone_noise_test)
add_executable(nonlinear_spectrum_test tests/nonlinear_spectrum_test.cpp)
target_link_libraries(nonlinear_spectrum_test PRIVATE rfmodel)
add_test(NAME nonlinear_spectrum_test COMMAND nonlinear_spectrum_test)
add_executable(power_wave_reference_test tests/power_wave_reference_test.cpp)
target_link_libraries(power_wave_reference_test PRIVATE rfmodel)
add_test(NAME power_wave_reference_test COMMAND power_wave_reference_test)
add_executable(intermod_levels_test tests/intermod_levels_test.cpp)
target_link_libraries(intermod_levels_test PRIVATE rfmodel)
add_test(NAME intermod_levels_test COMMAND intermod_levels_test)
add_executable(polynomial_intercepts_test tests/polynomial_intercepts_test.cpp)
target_link_libraries(polynomial_intercepts_test PRIVATE rfmodel)
add_test(NAME polynomial_intercepts_test COMMAND polynomial_intercepts_test)
add_executable(polynomial_amplifier_test tests/polynomial_amplifier_test.cpp)
target_link_libraries(polynomial_amplifier_test PRIVATE rfmodel)
add_test(NAME polynomial_amplifier_test COMMAND polynomial_amplifier_test)
add_executable(fundamental_compression_test tests/fundamental_compression_test.cpp)
target_link_libraries(fundamental_compression_test PRIVATE rfmodel)
add_test(NAME fundamental_compression_test COMMAND fundamental_compression_test)
add_executable(saturating_fundamental_test tests/saturating_fundamental_test.cpp)
target_link_libraries(saturating_fundamental_test PRIVATE rfmodel)
add_test(NAME saturating_fundamental_test COMMAND saturating_fundamental_test)
add_executable(single_tone_amplifier_test tests/single_tone_amplifier_test.cpp)
target_link_libraries(single_tone_amplifier_test PRIVATE rfmodel)
add_test(NAME single_tone_amplifier_test COMMAND single_tone_amplifier_test)
add_executable(multitone_amplifier_test tests/multitone_amplifier_test.cpp)
target_link_libraries(multitone_amplifier_test PRIVATE rfmodel)
add_test(NAME multitone_amplifier_test COMMAND multitone_amplifier_test)
add_executable(origin_expression_test tests/origin_expression_test.cpp)
target_link_libraries(origin_expression_test PRIVATE rfmodel)
add_test(NAME origin_expression_test COMMAND origin_expression_test)

add_executable(mixing_origin_test tests/mixing_origin_test.cpp)
target_link_libraries(mixing_origin_test PRIVATE rfmodel)
add_test(NAME mixing_origin_test COMMAND mixing_origin_test)
add_executable(coherent_highorder_amplifier_test tests/coherent_highorder_amplifier_test.cpp)
target_link_libraries(coherent_highorder_amplifier_test PRIVATE rfmodel)
add_test(NAME coherent_highorder_amplifier_test COMMAND coherent_highorder_amplifier_test)
add_executable(coherent_polynomial_test tests/coherent_polynomial_test.cpp)
target_link_libraries(coherent_polynomial_test PRIVATE rfmodel)
add_test(NAME coherent_polynomial_test COMMAND coherent_polynomial_test)
add_executable(amplifier_operating_point_test tests/amplifier_operating_point_test.cpp)
target_link_libraries(amplifier_operating_point_test PRIVATE rfmodel)
add_test(NAME amplifier_operating_point_test COMMAND amplifier_operating_point_test)

add_executable(coherent_amplifier_test tests/coherent_amplifier_test.cpp)
target_link_libraries(coherent_amplifier_test PRIVATE rfmodel)
add_test(NAME coherent_amplifier_test COMMAND coherent_amplifier_test)
add_executable(coherent_compression_test tests/coherent_compression_test.cpp)
target_link_libraries(coherent_compression_test PRIVATE rfmodel)
add_test(NAME coherent_compression_test COMMAND coherent_compression_test)

add_executable(coherent_mixer_test tests/coherent_mixer_test.cpp)
target_link_libraries(coherent_mixer_test PRIVATE rfmodel)
add_test(NAME coherent_mixer_test COMMAND coherent_mixer_test)
add_executable(source_coherence_test tests/source_coherence_test.cpp)
target_link_libraries(source_coherence_test PRIVATE rfmodel)
add_test(NAME source_coherence_test COMMAND source_coherence_test)
add_executable(coherent_network_test tests/coherent_network_test.cpp)
target_link_libraries(coherent_network_test PRIVATE rfmodel)
add_test(NAME coherent_network_test COMMAND coherent_network_test)
add_executable(coherence_test tests/coherence_test.cpp)
target_link_libraries(coherence_test PRIVATE rfmodel)
add_test(NAME coherence_test COMMAND coherence_test)
add_executable(term_propagation_test tests/term_propagation_test.cpp)
target_link_libraries(term_propagation_test PRIVATE rfmodel)
add_test(NAME term_propagation_test COMMAND term_propagation_test)
add_executable(ideal_mixer_test tests/ideal_mixer_test.cpp)
target_link_libraries(ideal_mixer_test PRIVATE rfmodel)
add_test(NAME ideal_mixer_test COMMAND ideal_mixer_test)
add_executable(spectrum_analysis_test tests/spectrum_analysis_test.cpp)
target_link_libraries(spectrum_analysis_test PRIVATE rfmodel)
add_test(NAME spectrum_analysis_test COMMAND spectrum_analysis_test)
add_executable(amplifier_model_test tests/amplifier_model_test.cpp)
target_link_libraries(amplifier_model_test PRIVATE rfmodel)
add_test(NAME amplifier_model_test COMMAND amplifier_model_test)
add_executable(power_gain_test tests/power_gain_test.cpp)
target_link_libraries(power_gain_test PRIVATE rfmodel)
add_test(NAME power_gain_test COMMAND power_gain_test)
add_executable(linear_path_test tests/linear_path_test.cpp)
target_link_libraries(linear_path_test PRIVATE rfmodel)
add_test(NAME linear_path_test COMMAND linear_path_test)
add_executable(reference_attenuator tests/reference_attenuator.cpp)
add_executable(linear_path_noise_test tests/linear_path_noise_test.cpp)
target_link_libraries(linear_path_noise_test PRIVATE rfmodel)
add_test(NAME linear_path_noise_test COMMAND linear_path_noise_test)
add_executable(loaded_noise_test tests/loaded_noise_test.cpp)
target_link_libraries(loaded_noise_test PRIVATE rfmodel)
add_test(NAME loaded_noise_test COMMAND loaded_noise_test)
target_link_libraries(reference_attenuator PRIVATE rfmodel)
add_executable(reference_antenna tests/reference_antenna.cpp)
target_link_libraries(reference_antenna PRIVATE rfmodel)
add_test(NAME reference_antenna_analytic COMMAND reference_antenna)
add_test(NAME reference_antenna_source_density COMMAND reference_antenna 6.903251499999993e-22)
endif()

add_executable(power_wave_noise_test tests/power_wave_noise_test.cpp)
target_link_libraries(power_wave_noise_test PRIVATE rfmodel)
add_test(NAME power_wave_noise_test COMMAND power_wave_noise_test)

add_executable(butterworth_filter_test tests/butterworth_filter_test.cpp)
target_link_libraries(butterworth_filter_test PRIVATE rfmodel)
add_test(NAME butterworth_filter_test COMMAND butterworth_filter_test)

add_executable(chebyshev_filter_test tests/chebyshev_filter_test.cpp)
target_link_libraries(chebyshev_filter_test PRIVATE rfmodel)
add_test(NAME chebyshev_filter_test COMMAND chebyshev_filter_test)

add_executable(frequency_conversion_test tests/frequency_conversion_test.cpp)
target_link_libraries(frequency_conversion_test PRIVATE rfmodel)
add_test(NAME frequency_conversion_test COMMAND frequency_conversion_test)

add_executable(conversion_network_test tests/conversion_network_test.cpp)
target_link_libraries(conversion_network_test PRIVATE rfmodel)
add_test(NAME conversion_network_test COMMAND conversion_network_test)

add_executable(conversion_loaded_noise_test tests/conversion_loaded_noise_test.cpp)
target_link_libraries(conversion_loaded_noise_test PRIVATE rfmodel)
add_test(NAME conversion_loaded_noise_test COMMAND conversion_loaded_noise_test)

add_executable(channel_noise_test tests/channel_noise_test.cpp)
target_link_libraries(channel_noise_test PRIVATE rfmodel)
add_test(NAME channel_noise_test COMMAND channel_noise_test)

add_executable(phase_noise_test tests/phase_noise_test.cpp)
target_link_libraries(phase_noise_test PRIVATE rfmodel)
add_test(NAME phase_noise_test COMMAND phase_noise_test)

add_executable(shared_phase_noise_test tests/shared_phase_noise_test.cpp)
target_link_libraries(shared_phase_noise_test PRIVATE rfmodel)
add_test(NAME shared_phase_noise_test COMMAND shared_phase_noise_test)

add_executable(mixer_linearization_test tests/mixer_linearization_test.cpp)
target_link_libraries(mixer_linearization_test PRIVATE rfmodel)
add_test(NAME mixer_linearization_test COMMAND mixer_linearization_test)

add_executable(affine_conversion_test tests/affine_conversion_test.cpp)
target_link_libraries(affine_conversion_test PRIVATE rfmodel)
add_test(NAME affine_conversion_test COMMAND affine_conversion_test)

add_executable(conversion_operating_point_test tests/conversion_operating_point_test.cpp)
target_link_libraries(conversion_operating_point_test PRIVATE rfmodel)
add_test(NAME conversion_operating_point_test COMMAND conversion_operating_point_test)

add_executable(amplifier_linearization_test tests/amplifier_linearization_test.cpp)
target_link_libraries(amplifier_linearization_test PRIVATE rfmodel)
add_test(NAME amplifier_linearization_test COMMAND amplifier_linearization_test)

add_executable(polynomial_linearization_test tests/polynomial_linearization_test.cpp)
target_link_libraries(polynomial_linearization_test PRIVATE rfmodel)
add_test(NAME polynomial_linearization_test COMMAND polynomial_linearization_test)

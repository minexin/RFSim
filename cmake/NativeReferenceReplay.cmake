# Developer reference replay only. Nothing in this file is installed/exported.
find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
add_executable(rfmodel_reference_highorder_probe tests/reference/reference_highorder_probe.cpp)
target_link_libraries(rfmodel_reference_highorder_probe PRIVATE RFModel::rfmodel)
add_executable(rfmodel_reference_highorder_oracle tests/reference/reference_highorder_oracle.cpp)
target_link_libraries(rfmodel_reference_highorder_oracle PRIVATE RFModel::rfmodel)
set_target_properties(rfmodel_reference_highorder_probe rfmodel_reference_highorder_oracle
    PROPERTIES CXX_EXTENSIONS OFF)

set(RFMODEL_REFERENCE_SOURCE_REVISION "" CACHE STRING
    "Source revision for archived/exported developer replay trees without git metadata")
set(reference_revision "${RFMODEL_REFERENCE_SOURCE_REVISION}")
if(NOT reference_revision)
    find_program(reference_git git)
    if(reference_git AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        execute_process(COMMAND "${reference_git}" rev-parse HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE reference_revision OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
endif()
if(NOT reference_revision)
    set(reference_revision "unavailable")
endif()
function(reference_json_string output value)
    string(REPLACE "\\" "\\\\" escaped "${value}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")
    string(REPLACE "\r" "\\r" escaped "${escaped}")
    string(REPLACE "\t" "\\t" escaped "${escaped}")
    set(${output} "\"${escaped}\"" PARENT_SCOPE)
endfunction()
file(GLOB_RECURSE reference_sources CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/rfmodel/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/*.py")
list(APPEND reference_sources CMakeLists.txt cmake/NativeReferenceReplay.cmake
    cmake/RFModelHeaders.cmake cmake/RFModelTests.cmake cmake/RFModelExamples.cmake
    tests/reference/reference_highorder_probe.cpp tests/reference/reference_highorder_oracle.cpp
    tests/cmake/check_native_replay_install.cmake tests/reference_consumer/CMakeLists.txt)
list(SORT reference_sources)
set(reference_files "")
set(reference_content "")
foreach(path IN LISTS reference_sources)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${path}")
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${path}" hash)
    string(APPEND reference_content "${path} ${hash}\n")
    if(reference_files)
        string(APPEND reference_files ",\n")
    endif()
    reference_json_string(quoted_path "${path}")
    string(APPEND reference_files "    ${quoted_path}: \"${hash}\"")
endforeach()
string(SHA256 reference_content_hash "${reference_content}")
reference_json_string(quoted_revision "${reference_revision}")
reference_json_string(quoted_compiler "${CMAKE_CXX_COMPILER}")
reference_json_string(quoted_generator "${CMAKE_GENERATOR}")
if(CMAKE_CONFIGURATION_TYPES)
    set(reference_configs ${CMAKE_CONFIGURATION_TYPES})
else()
    set(reference_configs "${CMAKE_BUILD_TYPE}")
    if(NOT reference_configs)
        set(reference_configs "unspecified")
    endif()
endif()
foreach(configuration IN LISTS reference_configs)
    string(TOUPPER "${configuration}" uppercase_config)
    reference_json_string(quoted_flags
        "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${uppercase_config}}; C++17; extensions=OFF")
    set(identity "  \"protocol\": 1,\n  \"source_revision\": ${quoted_revision},\n  \"source_content_sha256\": \"${reference_content_hash}\",\n  \"configuration\": \"${configuration}\",\n  \"compiler\": {\"id\": \"${CMAKE_CXX_COMPILER_ID}\", \"version\": \"${CMAKE_CXX_COMPILER_VERSION}\", \"path\": ${quoted_compiler}, \"flags\": ${quoted_flags}},\n  \"generator\": ${quoted_generator},\n  \"source_files\": {\n${reference_files}\n  }")
    string(SHA256 build_id "${identity}")
    set(manifest "{\n  \"build_id\": \"${build_id}\",\n${identity}\n}\n")
    if(configuration STREQUAL "unspecified")
        target_compile_definitions(rfmodel_reference_highorder_probe PRIVATE
            "RFMODEL_NATIVE_REPLAY_BUILD_ID=\"${build_id}\"")
        file(GENERATE OUTPUT "$<TARGET_FILE:rfmodel_reference_highorder_probe>.manifest.json"
            CONTENT "${manifest}" NEWLINE_STYLE LF)
    else()
        target_compile_definitions(rfmodel_reference_highorder_probe PRIVATE
            "$<$<CONFIG:${configuration}>:RFMODEL_NATIVE_REPLAY_BUILD_ID=\"${build_id}\">")
        file(GENERATE OUTPUT "$<TARGET_FILE:rfmodel_reference_highorder_probe>.manifest.json"
            CONTENT "${manifest}" CONDITION "$<CONFIG:${configuration}>" NEWLINE_STYLE LF)
    endif()
endforeach()

add_test(NAME native_replay_protocol COMMAND "${Python3_EXECUTABLE}" -I -S
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_native_replay.py"
    $<TARGET_FILE:rfmodel_reference_highorder_probe>
    $<TARGET_FILE:rfmodel_reference_highorder_oracle>)
foreach(suite IN ITEMS highorder_model_reference highorder_input_reference
        diagnose_eleventh_order diagnose_highorder_amplifier diagnose_spectrum_reduction
        diagnose_am_pm_reference diagnose_am_pm_parameter_study systemvue_am_pm_adapter)
    add_test(NAME native_${suite} COMMAND "${Python3_EXECUTABLE}" -I -S
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_${suite}.py"
        --native-probe $<TARGET_FILE:rfmodel_reference_highorder_probe>)
    set_tests_properties(native_${suite} PROPERTIES TIMEOUT 180)
endforeach()
# Acquisition guards and frozen hypothesis scoring use only archived JSON and
# Python's standard library; they do not accept a native-probe argument.
foreach(suite IN ITEMS rfamp_parameter_pilot archive_am_pm_datapoints_zero_study
        archive_am_pm_datapoints_zero_acquisition diagnose_am_pm_datapoints_zero_study
        prepare_am_pm_datapoints_raw_axis_study diagnose_am_pm_datapoints_raw_axis_study
        archive_am_pm_datapoints_raw_axis_acquisition report_am_pm_datapoints_raw_axis_study
        prepare_am_pm_datapoints_order_study diagnose_am_pm_datapoints_order_study
        archive_am_pm_datapoints_order_acquisition report_am_pm_datapoints_order_study
        prepare_am_pm_datapoints_allocation_study normalize_am_pm_datapoints_allocation
        diagnose_am_pm_datapoints_allocation_study archive_am_pm_datapoints_allocation_acquisition
        report_am_pm_datapoints_allocation_study)
    add_test(NAME native_${suite} COMMAND "${Python3_EXECUTABLE}" -I -S
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/reference/test_${suite}.py")
    set_tests_properties(native_${suite} PROPERTIES TIMEOUT 180)
endforeach()
set_tests_properties(native_replay_protocol PROPERTIES TIMEOUT 240)
add_test(NAME native_replay_installed_sdk COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}/reference-installed-sdk"
    "-DGENERATOR=${CMAKE_GENERATOR}" "-DCOMPILER=${CMAKE_CXX_COMPILER}"
    "-DCONFIG=$<CONFIG>" "-DPYTHON=${Python3_EXECUTABLE}"
    "-DSOURCE_REVISION=${reference_revision}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_native_replay_install.cmake")
set_tests_properties(native_replay_installed_sdk PROPERTIES TIMEOUT 300)

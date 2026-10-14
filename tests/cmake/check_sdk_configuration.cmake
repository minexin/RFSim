# Ensure SDK-only consumers do not accidentally build tests or discover Python.
if(NOT DEFINED SOURCE OR NOT DEFINED BINARY OR NOT DEFINED GENERATOR)
    message(FATAL_ERROR "SOURCE, BINARY and GENERATOR are required")
endif()
function(project_targets output model configuration reply_directory)
    string(JSON count LENGTH "${model}" configurations ${configuration} targets)
    set(actual)
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(target RANGE 0 ${last})
            string(JSON file GET "${model}" configurations ${configuration} targets ${target} jsonFile)
            file(READ "${reply_directory}/${file}" detail)
            string(JSON generator_provided ERROR_VARIABLE generator_error
                GET "${detail}" isGeneratorProvided)
            if(NOT generator_error AND generator_provided)
                continue()
            endif()
            string(JSON type GET "${detail}" type)
            if(type STREQUAL "INTERFACE_LIBRARY")
                continue()
            endif()
            string(JSON name GET "${detail}" name)
            list(APPEND actual "${name}")
        endforeach()
    endif()
    list(SORT actual)
    set(${output} "${actual}" PARENT_SCOPE)
endfunction()
# Visual Studio/Xcode expose generator-provided ALL_BUILD/ZERO_CHECK objects.
# Use their documented flag, not names, and ignore non-buildable interfaces.
# The synthetic fixture also ensures ordinary user utility targets stay visible.
set(fixture "${BINARY}/target-filter-fixture")
file(MAKE_DIRECTORY "${fixture}")
file(WRITE "${fixture}/all.json" "{\"name\":\"ALL_BUILD\",\"type\":\"UTILITY\",\"isGeneratorProvided\":true}")
file(WRITE "${fixture}/zero.json" "{\"name\":\"ZERO_CHECK\",\"type\":\"UTILITY\",\"isGeneratorProvided\":true}")
file(WRITE "${fixture}/interface.json" "{\"name\":\"rfmodel\",\"type\":\"INTERFACE_LIBRARY\"}")
file(WRITE "${fixture}/user.json" "{\"name\":\"user_probe\",\"type\":\"UTILITY\",\"isGeneratorProvided\":false}")
set(model "{\"configurations\":[{\"targets\":[{\"jsonFile\":\"all.json\"},{\"jsonFile\":\"zero.json\"},{\"jsonFile\":\"interface.json\"},{\"jsonFile\":\"user.json\"}]}]}")
project_targets(filtered "${model}" 0 "${fixture}")
if(NOT filtered STREQUAL "user_probe")
    message(FATAL_ERROR "Generator/interface target filtering failed: ${filtered}")
endif()

function(check_targets suffix examples expected_count)
    set(directory "${BINARY}/${suffix}")
    file(MAKE_DIRECTORY "${directory}/.cmake/api/v1/query")
    file(WRITE "${directory}/.cmake/api/v1/query/codemodel-v2" "")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${directory}"
        -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${COMPILER}"
        -DBUILD_TESTING=OFF -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF
        "-DRFMODEL_BUILD_EXAMPLES=${examples}" -DRFMODEL_BUILD_PYTHON_TESTS=OFF
        -DRFMODEL_BUILD_REFERENCE_TESTS=OFF
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "SDK configure failed: ${output}\n${error}")
    endif()
    file(GLOB replies "${directory}/.cmake/api/v1/reply/codemodel-v2-*.json")
    list(LENGTH replies reply_count)
    if(NOT reply_count EQUAL 1)
        message(FATAL_ERROR "Expected one codemodel reply")
    endif()
    list(GET replies 0 reply)
    file(READ "${reply}" model)
    string(JSON configurations LENGTH "${model}" configurations)
    math(EXPR last_configuration "${configurations} - 1")
    foreach(index RANGE 0 ${last_configuration})
        project_targets(actual "${model}" ${index} "${directory}/.cmake/api/v1/reply")
        list(LENGTH actual count)
        if(NOT count EQUAL expected_count)
            message(FATAL_ERROR "SDK target count ${count}, expected ${expected_count}: ${actual}")
        endif()
        if(examples)
            set(expected coherent_pipeline_example frequency_dependent_cable_example mixed_reference_conversion_example model_adapter_example rfmodel_correlated_tolerance
                rfmodel_match_design rfmodel_tolerance_analysis)
            if(NOT actual STREQUAL expected)
                message(FATAL_ERROR "Unexpected example targets: ${actual}")
            endif()
        endif()
    endforeach()
    file(READ "${directory}/CMakeCache.txt" cache)
    if(cache MATCHES "Python3_EXECUTABLE" OR cache MATCHES "CMAKE_C_COMPILER:")
        message(FATAL_ERROR "Pure C++ SDK unexpectedly discovered Python or enabled C")
    endif()
endfunction()
check_targets(minimal OFF 0)
check_targets(examples ON 7)
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${BINARY}/conflict"
    -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${COMPILER}"
    -DBUILD_TESTING=OFF -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF
    -DRFMODEL_BUILD_EXAMPLES=OFF -DRFMODEL_BUILD_PYTHON_TESTS=OFF
    -DRFMODEL_REQUIRE_PYTHON_TESTS=ON
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT "${output}${error}" MATCHES "Python product interfaces have been retired")
    message(FATAL_ERROR "Conflicting required Python tests did not fail explicitly")
endif()

# Explicit developer replay must never be silently skipped.
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${BINARY}/reference-conflict"
    -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${COMPILER}"
    -DBUILD_TESTING=OFF -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_PYTHON_TESTS=OFF
    -DRFMODEL_BUILD_REFERENCE_TESTS=ON
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT "${output}${error}" MATCHES "Native reference tests need BUILD_TESTING")
    message(FATAL_ERROR "Conflicting reference test option did not fail explicitly")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${BINARY}/reference-no-python"
    -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${COMPILER}"
    -DBUILD_TESTING=ON -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_PYTHON_TESTS=OFF
    -DRFMODEL_BUILD_REFERENCE_TESTS=ON -DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(status EQUAL 0 OR NOT "${output}${error}" MATCHES "Python3")
    message(FATAL_ERROR "Explicit reference tests silently skipped missing Python")
endif()

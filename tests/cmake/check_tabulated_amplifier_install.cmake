# Independent relocated installations; consumers have no source include fallback.
function(run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Tabulated amplifier installed check failed (${status}): ${ARGV}\n${output}\n${error}")
    endif()
endfunction()
foreach(mode IN ITEMS full pure)
    set(c_api ON)
    if(mode STREQUAL "pure")
        set(c_api OFF)
    endif()
    set(root "${BINARY}/${mode}")
    run("${CMAKE_COMMAND}" -S "${SOURCE}" -B "${root}/build" -G "${GENERATOR}"
        "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
        -DCMAKE_CXX_EXTENSIONS=OFF -DBUILD_TESTING=OFF "-DRFMODEL_BUILD_C_API=${c_api}"
        "-DRFMODEL_BUILD_CLI=${c_api}" -DRFMODEL_BUILD_EXAMPLES=OFF -DRFMODEL_BUILD_REFERENCE_TESTS=OFF)
    run("${CMAKE_COMMAND}" --build "${root}/build" --config "${CONFIG}" --parallel 2)
    run("${CMAKE_COMMAND}" --install "${root}/build" --config "${CONFIG}" --prefix "${root}/prefix")
    file(REMOVE_RECURSE "${root}/relocated")
    file(RENAME "${root}/prefix" "${root}/relocated")
    if(mode STREQUAL "pure")
        file(READ "${root}/build/CMakeCache.txt" cache)
        if(cache MATCHES "CMAKE_C_COMPILER:|Python3_EXECUTABLE")
            message(FATAL_ERROR "Pure SDK enabled C or Python")
        endif()
    endif()
    file(COPY "${SOURCE}/tests/tabulated_amplifier_consumer/" DESTINATION "${root}/consumer")
    run("${CMAKE_COMMAND}" -S "${root}/consumer" -B "${root}/consumer-build" -G "${GENERATOR}"
        "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
        "-DCMAKE_PREFIX_PATH=${root}/relocated")
    run("${CMAKE_COMMAND}" --build "${root}/consumer-build" --config "${CONFIG}" --parallel 2)
    run("${CMAKE_CTEST_COMMAND}" --test-dir "${root}/consumer-build" -C "${CONFIG}" --output-on-failure)
endforeach()

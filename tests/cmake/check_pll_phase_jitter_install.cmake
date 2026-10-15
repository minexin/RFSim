# Pure C++ header SDK and copied dual-TU example: no source include fallback.
function(run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "PLL phase jitter installed check failed (${status}): ${ARGV}\n${output}\n${error}")
    endif()
endfunction()
if(NOT CONFIG)
    set(CONFIG Release)
endif()
run("${CMAKE_COMMAND}" -S "${SOURCE}" -B "${BINARY}/producer" -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    -DCMAKE_CXX_EXTENSIONS=OFF -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTING=OFF -DRFMODEL_BUILD_C_API=OFF
    -DRFMODEL_BUILD_CLI=OFF -DRFMODEL_BUILD_EXAMPLES=OFF -DRFMODEL_BUILD_REFERENCE_TESTS=OFF)
# No producer compilation is necessary for this header-only install.
run("${CMAKE_COMMAND}" --install "${BINARY}/producer" --config "${CONFIG}" --prefix "${BINARY}/prefix")
file(REMOVE_RECURSE "${BINARY}/relocated")
file(RENAME "${BINARY}/prefix" "${BINARY}/relocated")
file(READ "${BINARY}/producer/CMakeCache.txt" cache)
if(cache MATCHES "CMAKE_C_COMPILER:|Python3_EXECUTABLE")
    message(FATAL_ERROR "Pure PLL example SDK enabled C or Python")
endif()
if(NOT EXISTS "${BINARY}/relocated/lib/cmake/RFModel/RFModelConfig.cmake")
    message(FATAL_ERROR "Relocated RFModel package is missing")
endif()
file(COPY "${SOURCE}/examples/pll-phase-jitter-sdk/" DESTINATION "${BINARY}/consumer")
run("${CMAKE_COMMAND}" -S "${BINARY}/consumer" -B "${BINARY}/consumer-build" -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    "-DRFModel_DIR=${BINARY}/relocated/lib/cmake/RFModel"
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF)
run("${CMAKE_COMMAND}" --build "${BINARY}/consumer-build" --config "${CONFIG}" --parallel 2)
run("${CMAKE_CTEST_COMMAND}" --test-dir "${BINARY}/consumer-build" -C "${CONFIG}" --output-on-failure)
message(STATUS "PLL phase jitter installed-only example: all three consumer tests passed (${CONFIG})")

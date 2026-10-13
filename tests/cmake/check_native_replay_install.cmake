# Separate SDK installation, relocation, and consumer; no source include fallback.
function(run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Installed replay check failed (${status}): ${ARGV}\n${output}\n${error}")
    endif()
endfunction()
if(NOT CONFIG)
    set(CONFIG Debug)
endif()
run("${CMAKE_COMMAND}" -S "${SOURCE}" -B "${BINARY}/sdk-build" -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    -DBUILD_TESTING=OFF -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF
    -DRFMODEL_BUILD_EXAMPLES=OFF -DRFMODEL_BUILD_PYTHON_TESTS=OFF
    -DRFMODEL_BUILD_REFERENCE_TESTS=OFF)
file(READ "${BINARY}/sdk-build/CMakeCache.txt" cache)
if(cache MATCHES "Python3_EXECUTABLE" OR cache MATCHES "CMAKE_C_COMPILER:")
    message(FATAL_ERROR "SDK unexpectedly enabled C or Python")
endif()
run("${CMAKE_COMMAND}" --build "${BINARY}/sdk-build" --config "${CONFIG}")
run("${CMAKE_COMMAND}" --install "${BINARY}/sdk-build" --config "${CONFIG}" --prefix "${BINARY}/install")
# The prefix is private to this check. Re-runs preserve the previous relocation.
file(REMOVE_RECURSE "${BINARY}/relocated")
file(RENAME "${BINARY}/install" "${BINARY}/relocated")
file(GLOB_RECURSE installed "${BINARY}/relocated/*")
foreach(path IN LISTS installed)
    get_filename_component(filename "${path}" NAME)
    if(filename MATCHES "\\.py$|probe|oracle|manifest\\.json$|\\.(so|dll|dylib)$")
        message(FATAL_ERROR "Developer/runtime artifact leaked into SDK: ${path}")
    endif()
endforeach()
file(MAKE_DIRECTORY "${BINARY}/consumer")
file(COPY "${SOURCE}/tests/reference_consumer/CMakeLists.txt"
    "${SOURCE}/tests/reference/reference_highorder_probe.cpp" "${SOURCE}/tests/reference/reference_highorder_oracle.cpp"
    DESTINATION "${BINARY}/consumer")
run("${CMAKE_COMMAND}" -S "${BINARY}/consumer" -B "${BINARY}/consumer-build" -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    "-DCMAKE_PREFIX_PATH=${BINARY}/relocated" "-DPYTHON=${PYTHON}"
    "-DSOURCE_REVISION=${SOURCE_REVISION}" "-DPROTOCOL_TEST=${SOURCE}/scripts/reference/test_native_replay.py")
run("${CMAKE_COMMAND}" --build "${BINARY}/consumer-build" --config "${CONFIG}" --parallel 2)
run("${CMAKE_CTEST_COMMAND}" --test-dir "${BINARY}/consumer-build" -C "${CONFIG}" --output-on-failure)

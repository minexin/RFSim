# Empty CONFIG must still perform both real relocated pure C++ SDK workflows.
# Keep child CONFIG empty even when the parent selects Debug or Release.
foreach(name IN ITEMS linear_mna mna_power_wave_s)
    set(root "${BINARY}/${name}")
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE=${SOURCE}" "-DBINARY=${root}"
        "-DGENERATOR=${GENERATOR}" "-DCOMPILER=${COMPILER}"
        -DCONFIG= -DMODES=pure
        -P "${SOURCE}/tests/cmake/check_${name}_install.cmake"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "Empty CONFIG ${name} installed check failed (${status}):\n${output}\n${error}")
    endif()
    # The child checks already install, relocate, compile two translation units,
    # and execute the consumer. Also prove the default reached both CMake caches.
    foreach(build IN ITEMS build consumer-build)
        file(STRINGS "${root}/pure/${build}/CMakeCache.txt" configured_build_type
            REGEX "^CMAKE_BUILD_TYPE:[^=]*=")
        if(NOT configured_build_type MATCHES "^CMAKE_BUILD_TYPE:[^=]*=Release$")
            message(FATAL_ERROR "Empty CONFIG ${name}/${build} did not default to Release: ${configured_build_type}")
        endif()
    endforeach()
    message(STATUS "Empty CONFIG ${name} installed check passed:\n${output}")
endforeach()

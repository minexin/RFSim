# A fresh test workspace retains failed evidence; never delete/overwrite old runs.
function(run)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Synthetic installed replay contract failed (${result}): ${ARGV}\n${output}\n${error}")
    endif()
    set(last_output "${output}${error}" PARENT_SCOPE)
endfunction()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(root "${BINARY}/run-${suffix}")
if(EXISTS "${root}")
    message(FATAL_ERROR "Refusing existing test workspace")
endif()
file(MAKE_DIRECTORY "${root}")
if(NOT CONFIG)
    set(CONFIG Release)
endif()
run("${CMAKE_COMMAND}" -S "${SOURCE}" -B "${root}/producer" -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    -DBUILD_TESTING=OFF -DRFMODEL_BUILD_C_API=OFF -DRFMODEL_BUILD_CLI=OFF
    -DRFMODEL_BUILD_EXAMPLES=OFF -DRFMODEL_BUILD_REFERENCE_TESTS=OFF)
run("${CMAKE_COMMAND}" --install "${root}/producer" --config "${CONFIG}"
    --prefix "${root}/installed SDK")
file(MAKE_DIRECTORY "${root}/consumer")
file(COPY "${SOURCE}/scripts/reference/probe-xparameter-data.cpp" DESTINATION "${root}/consumer")
file(COPY "${SOURCE}/scripts/reference/installed-xparameter-probe/CMakeLists.txt"
    "${SOURCE}/scripts/reference/installed-xparameter-probe/odr.cpp" DESTINATION "${root}/consumer")
run("${CMAKE_COMMAND}" -S "${root}/consumer" -B "${root}/consumer-build" -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
    "-DSDK_PREFIX=${root}/installed SDK")
run("${CMAKE_COMMAND}" --build "${root}/consumer-build" --config "${CONFIG}" --parallel 2)
set(executable "${root}/consumer-build/bin/xparameter_replay_probe")
if(WIN32)
    string(APPEND executable ".exe")
endif()
run("${CMAKE_COMMAND}" -E env "RFMODEL_XPARAMETER_PROBE=${executable}"
    "RFMODEL_XPARAMETER_SDK=${root}/installed SDK"
    "RFMODEL_XPARAMETER_CMAKE=${CMAKE_COMMAND}"
    "RFMODEL_XPARAMETER_COMPILER=${COMPILER}"
    "RFMODEL_XPARAMETER_CONFIG=${CONFIG}"
    "RFMODEL_XPARAMETER_GENERATOR=${GENERATOR}"
    "PYTHONDONTWRITEBYTECODE=1"
    "${PYTHON}" "${SOURCE}/scripts/reference/test_xparameter_installed_replay.py")
message(STATUS "${last_output}")
message(STATUS "Synthetic installed SDK/wrapper/scorer validation passed; zero vendor simulations")

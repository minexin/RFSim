# Opt-in scripts only: no build targets or packaging runtime dependencies.
if(RFMODEL_BUILD_C_API OR RFMODEL_BUILD_CLI OR RFMODEL_BUILD_EXAMPLES OR BUILD_TESTING)
    message(FATAL_ERROR "SDK packaging requires C API, CLI, examples and BUILD_TESTING OFF")
endif()
if(NOT CMAKE_INSTALL_LIBDIR STREQUAL "lib")
    message(FATAL_ERROR "SDK packaging requires CMAKE_INSTALL_LIBDIR=lib")
endif()
if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "SDK archive production requires a native build; extracted headers support cross consumers")
endif()
find_package(Git REQUIRED)
set(sdk_payload
    lib/cmake/RFModel/RFModelConfig.cmake
    lib/cmake/RFModel/RFModelConfigVersion.cmake
    lib/cmake/RFModel/RFModelTargets.cmake
    examples/sdk-consumer/CMakeLists.txt
    examples/sdk-consumer/main.cpp
    examples/sdk-consumer/network.cpp
    examples/sdk-consumer/README.md
    README.md verify-sdk.cmake)
set(sdk_inputs CMakeLists.txt cmake/RFModelHeaders.cmake cmake/RFModelConfig.cmake.in
    cmake/RFModelPackaging.cmake cmake/sdk/PackageSDK.cmake.in
    cmake/sdk/VerifySDK.cmake.in cmake/sdk/ExtractSDK.cmake cmake/sdk/README.md
    cmake/sdk/Producer.cmake.in
    examples/sdk-consumer/CMakeLists.txt examples/sdk-consumer/main.cpp
    examples/sdk-consumer/network.cpp examples/sdk-consumer/README.md)
foreach(header IN LISTS RFMODEL_PUBLIC_HEADERS)
    get_filename_component(flat "${header}" NAME)
    list(APPEND sdk_payload "include/rfmodel/${header}" "include/rfmodel/${flat}")
    list(APPEND sdk_inputs "include/rfmodel/${header}")
endforeach()
list(SORT sdk_payload)
list(SORT sdk_inputs)
set(sdk_input_hashes)
foreach(input IN LISTS sdk_inputs)
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${input}" hash)
    list(APPEND sdk_input_hashes "${hash}")
endforeach()
configure_file(cmake/sdk/VerifySDK.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/VerifySDK.cmake" @ONLY)
configure_file(cmake/sdk/PackageSDK.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/PackageSDK.cmake" @ONLY)
configure_file(cmake/sdk/ExtractSDK.cmake "${CMAKE_CURRENT_BINARY_DIR}/ExtractSDK.cmake" @ONLY)
configure_file(cmake/sdk/Producer.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/SDKProducer.cmake" @ONLY)

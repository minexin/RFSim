cmake_minimum_required(VERSION 3.20)
project(RFModel VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_library(rfmodel INTERFACE)
add_library(RFModel::rfmodel ALIAS rfmodel)
target_compile_features(rfmodel INTERFACE cxx_std_17)
include(cmake/RFModelHeaders.cmake)
target_include_directories(rfmodel INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${RFMODEL_LEGACY_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:include>)
include(GNUInstallDirs)
option(RFMODEL_BUILD_C_API "Build the shared C ABI for linear networks" ON)
option(RFMODEL_BUILD_CLI "Build the batch S parameter CSV exporter" ON)
if(RFMODEL_BUILD_C_API)
    enable_language(C)
    add_library(rfmodel_c SHARED src/api/c_api.cpp)
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
install(DIRECTORY include/rfmodel DESTINATION include FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
install(DIRECTORY "${RFMODEL_LEGACY_INCLUDE_DIR}/rfmodel" DESTINATION include
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h")
install(EXPORT RFModelTargets NAMESPACE RFModel:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RFModel)
configure_package_config_file(cmake/RFModelConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/RFModelConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RFModel)
set(rfmodel_version_arch)
if(NOT RFMODEL_BUILD_C_API AND NOT RFMODEL_BUILD_CLI)
    set(rfmodel_version_arch ARCH_INDEPENDENT)
endif()
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/RFModelConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion ${rfmodel_version_arch})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/RFModelConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/RFModelConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RFModel)
include(CTest)
option(RFMODEL_BUILD_EXAMPLES "Build standalone C++ examples" ON)
option(RFMODEL_BUILD_REFERENCE_TESTS "Build developer-only native archived-reference replay tests" OFF)
if(RFMODEL_BUILD_REFERENCE_TESTS AND NOT BUILD_TESTING)
    message(FATAL_ERROR "Native reference tests need BUILD_TESTING")
endif()
# Old OFF flags are accepted for existing native SDK build scripts. Explicit ON
# must fail rather than silently pretending that the removed bindings were tested.
if(RFMODEL_BUILD_PYTHON_TESTS OR RFMODEL_REQUIRE_PYTHON_TESTS)
    message(FATAL_ERROR "Python product interfaces have been retired; use RFMODEL_BUILD_REFERENCE_TESTS for independent developer tools")
endif()
if(RFMODEL_BUILD_CLI)
add_executable(rfmodel_cli src/cli/rfmodel_cli.cpp)
target_link_libraries(rfmodel_cli PRIVATE RFModel::rfmodel)
install(TARGETS rfmodel_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(BUILD_TESTING)
    include(cmake/RFModelTests.cmake)
endif()

if(RFMODEL_BUILD_EXAMPLES)
    include(cmake/RFModelExamples.cmake)
endif()

option(RFMODEL_ENABLE_SDK_PACKAGING "Generate scripts for internal header-only SDK archives" OFF)
if(RFMODEL_ENABLE_SDK_PACKAGING)
    include(cmake/RFModelPackaging.cmake)
endif()

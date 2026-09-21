cmake_minimum_required(VERSION 3.25)

project(FCFN
  VERSION 1.0.0
  DESCRIPTION "Failure Containment Fabric Network - bounded failure-containment intent runtime"
  HOMEPAGE_URL "https://github.com/summonlabs/Failure-Containment-Fabric-Network"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(FCFN_IS_TOP_LEVEL ON)
else()
  set(FCFN_IS_TOP_LEVEL OFF)
endif()

option(FCFN_BUILD_TESTS "Build the FCFN proof suites" ${FCFN_IS_TOP_LEVEL})
option(FCFN_BUILD_APPS "Build the FCFN executables (coordinator, client, ctl)" ${FCFN_IS_TOP_LEVEL})
option(FCFN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(FCFN_ENABLE_ASAN "Enable AddressSanitizer where the toolchain supports it" OFF)
option(FCFN_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer where the toolchain supports it" OFF)
option(FCFN_ENABLE_STATIC_ANALYSIS "Enable the compiler's static analysis pass where available" OFF)

# ---------------------------------------------------------------------------
# Language settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Apply the project's warning, sanitizer, and analysis settings to a target.
# Implemented as a function rather than an INTERFACE library so installed
# packages never reference build-only helper targets.
function(fcfn_apply_build_flags target)
  if(MSVC)
    # FCFN uses the portable C stdio API with its own bounds checking; the MSVC
    # deprecation shim is not applicable.
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /EHsc)
    if(FCFN_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(FCFN_ENABLE_STATIC_ANALYSIS)
      target_compile_options(${target} PRIVATE /analyze)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(FCFN_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(FCFN_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
  if(FCFN_ENABLE_UBSAN)
    if(MSVC)
      message(FATAL_ERROR "FCFN_ENABLE_UBSAN is UNSUPPORTED with MSVC; use a Clang/GCC toolchain")
    else()
      target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=undefined)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(FCFN_BUILD_APPS)
  add_subdirectory(apps)
endif()

if(FCFN_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
install(EXPORT FCFNTargets
  FILE FCFNTargets.cmake
  NAMESPACE fcfn::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FCFN)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FCFNConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/FCFNConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FCFN)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/FCFNConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/FCFNConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/FCFNConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FCFN)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "FCFN ${PROJECT_VERSION} | build type: ${CMAKE_BUILD_TYPE} | tests: ${FCFN_BUILD_TESTS} | apps: ${FCFN_BUILD_APPS}")
message(STATUS "FCFN | ASan: ${FCFN_ENABLE_ASAN} | UBSan: ${FCFN_ENABLE_UBSAN} | static analysis: ${FCFN_ENABLE_STATIC_ANALYSIS}")

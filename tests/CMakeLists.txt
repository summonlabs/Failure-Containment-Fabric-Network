# FCFN proof suites.
#
# Each subdirectory is one executable. Sources are globbed with CONFIGURE_DEPENDS
# so the suite layout stays declarative. No test declares a timeout: a hang is a
# defect to diagnose, never something to mask with a watchdog.

add_library(fcfn_test_support STATIC
  support/test_harness.cpp
  support/temp_dir.cpp
  support/process.cpp
  support/fixtures.cpp
  support/reference_solver.cpp)
target_include_directories(fcfn_test_support PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_definitions(fcfn_test_support PUBLIC
  FCFN_TEST_BUILD_ROOT="${CMAKE_BINARY_DIR}"
  FCFN_TEST_APP_DIR="${CMAKE_BINARY_DIR}/apps"
  FCFN_TEST_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
target_link_libraries(fcfn_test_support PUBLIC fcfn::core)
fcfn_apply_build_flags(fcfn_test_support)

set(FCFN_TEST_SUITES
  unit
  codec
  persistence
  solver
  property
  adversarial
  concurrency
  multiprocess
  scale)

foreach(suite IN LISTS FCFN_TEST_SUITES)
  file(GLOB suite_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${suite}/*.cpp")
  if(NOT suite_sources)
    continue()
  endif()
  set(target "fcfn_test_${suite}")
  add_executable(${target} ${suite_sources})
  target_link_libraries(${target} PRIVATE fcfn_test_support)
  if(suite STREQUAL "multiprocess" OR suite STREQUAL "concurrency")
    # Real sockets, real sessions, and the coordinator service are part of these
    # proofs even when they run inside one process.
    target_link_libraries(${target} PRIVATE fcfn::runtime)
  endif()
  fcfn_apply_build_flags(${target})
  add_test(NAME ${suite} COMMAND ${target} --seed=20260101)
  set_tests_properties(${suite} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()

# The downstream consumer fixture lives in examples/downstream_consumer and is
# configured and built OUTSIDE this source tree against an installed prefix by
# scripts/validate_install.ps1. It is deliberately not part of this build.

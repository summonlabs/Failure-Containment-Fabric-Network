# FCFN core library: domain model, containment engine, durable store, runtime.
add_library(fcfn_core STATIC
  core/status.cpp
  core/ids.cpp
  core/hash.cpp
  core/json.cpp
  core/clock.cpp
  model/explanation.cpp
  model/topology.cpp
  model/evidence.cpp
  model/policy.cpp
  model/authority.cpp
  model/boundary.cpp
  model/plan.cpp
  model/attempt.cpp
  model/transition.cpp
  engine/graph.cpp
  engine/cut.cpp
  engine/solver_exact.cpp
  engine/solver_heuristic.cpp
  engine/necessity.cpp
  engine/planner.cpp
  store/record.cpp
  store/store.cpp
  runtime/codec.cpp
  runtime/coordinator.cpp
  runtime/executor.cpp)

add_library(fcfn::core ALIAS fcfn_core)

target_include_directories(fcfn_core
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

fcfn_apply_build_flags(fcfn_core)
set_target_properties(fcfn_core PROPERTIES
  EXPORT_NAME core
  OUTPUT_NAME fcfn_core
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON)

# FCFN runtime library: framed transport, sessions, coordinator service client.
add_library(fcfn_runtime STATIC
  net/frame.cpp
  net/messages.cpp
  net/socket.cpp
  net/client.cpp
  net/server.cpp)

add_library(fcfn::runtime ALIAS fcfn_runtime)
target_include_directories(fcfn_runtime
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fcfn_runtime PUBLIC fcfn_core)
fcfn_apply_build_flags(fcfn_runtime)
set_target_properties(fcfn_runtime PROPERTIES
  EXPORT_NAME runtime
  OUTPUT_NAME fcfn_runtime
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON)

if(WIN32)
  target_link_libraries(fcfn_runtime PRIVATE ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(fcfn_runtime PUBLIC Threads::Threads)
endif()

install(TARGETS fcfn_core fcfn_runtime
  EXPORT FCFNTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

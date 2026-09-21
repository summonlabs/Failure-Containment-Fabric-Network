# FCFN executables. All of them are thin shells over the installed library.

add_library(fcfn_app_support STATIC
  support/config_text.cpp)
target_include_directories(fcfn_app_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_link_libraries(fcfn_app_support PUBLIC fcfn::core)
fcfn_apply_build_flags(fcfn_app_support)

add_executable(fcfn_coordinator coordinator/main.cpp)
target_link_libraries(fcfn_coordinator PRIVATE fcfn_app_support fcfn::runtime)
fcfn_apply_build_flags(fcfn_coordinator)

add_executable(fcfn_client client/main.cpp)
target_link_libraries(fcfn_client PRIVATE fcfn_app_support fcfn::runtime)
fcfn_apply_build_flags(fcfn_client)

add_executable(fcfnctl ctl/main.cpp)
target_link_libraries(fcfnctl PRIVATE fcfn_app_support fcfn::core)
fcfn_apply_build_flags(fcfnctl)

install(TARGETS fcfn_coordinator fcfn_client fcfnctl
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

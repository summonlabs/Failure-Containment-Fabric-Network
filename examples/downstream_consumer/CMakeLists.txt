cmake_minimum_required(VERSION 3.25)

# Independent consumer project. It is configured and built OUTSIDE the FCFN
# source tree against an installed prefix, using only find_package(FCFN CONFIG).
project(fcfn_consumer LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(FCFN 1.0 CONFIG REQUIRED)

message(STATUS "consumer: FCFN ${FCFN_VERSION} found")

add_executable(fcfn_consumer main.cpp)
target_link_libraries(fcfn_consumer PRIVATE fcfn::core fcfn::runtime)
if(MSVC)
  target_compile_options(fcfn_consumer PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
else()
  target_compile_options(fcfn_consumer PRIVATE -Wall -Wextra -Werror)
endif()

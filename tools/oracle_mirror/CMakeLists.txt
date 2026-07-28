# Clean-room side of the M3 oracle diff harness (Part B). Scratch tool: build it
# standalone against the sim sources, or add this dir with add_subdirectory from
# a scratch build that already defines the bomber::sim target.
#
# Standalone build (repo root):
#   cmake -S tools/oracle_mirror -B build/oracle_mirror -DBOMBER_SIM_ROOT=libs/sim
#   cmake --build build/oracle_mirror
cmake_minimum_required(VERSION 3.20)
project(oracle_mirror CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point at the clean-room sim. Default assumes this dir sits at <repo>/tools/oracle_mirror.
set(BOMBER_SIM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/sim" CACHE PATH "libs/sim path")
# libs/sim's public headers include bomber/core/geometry.hpp (ADR-0008), so the
# standalone build needs libs/core on the include path too.
set(BOMBER_CORE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../libs/core" CACHE PATH "libs/core path")

file(GLOB SIM_SYS "${BOMBER_SIM_ROOT}/src/systems/*.cpp")
add_executable(mirror
    mirror.cpp
    "${BOMBER_SIM_ROOT}/src/setup.cpp"
    "${BOMBER_SIM_ROOT}/src/hash.cpp"
    "${BOMBER_SIM_ROOT}/src/simulation.cpp"
    ${SIM_SYS})
target_include_directories(mirror PRIVATE
    "${BOMBER_SIM_ROOT}/include"
    "${BOMBER_SIM_ROOT}/src"
    "${BOMBER_CORE_ROOT}/include")

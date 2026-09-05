# Thunderbolt HT - the parallel execution runtime.
#
# This directory is configurable BOTH as a subdirectory of the repo root and as a
# standalone project:
#
#     cmake -S thunderbolt -B build/standalone
#
# The standalone path is a hard requirement (S4: "Thunderbolt HT can be built
# independently") and is enforced by a test, not just by convention. Nothing here
# may reference engine/, game/ or the repo-root cmake/ directory.

cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(THUNDERBOLT_STANDALONE ON)
    project(Thunderbolt VERSION 0.1.0 LANGUAGES CXX)
else()
    set(THUNDERBOLT_STANDALONE OFF)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ThunderboltCompilerFlags)

set(THUNDERBOLT_VERSION_STRING "0.1.0")

add_library(thunderbolt STATIC
    core/ShardedCounter.cpp
    core/task/TaskPool.cpp
    core/taskgraph/TaskGraph.cpp
    cpu/affinity/Affinity.cpp
    cpu/topology/CpuTopology.cpp
    runtime/BuildInfo.cpp
    runtime/RuntimeBase.cpp
    runtime/StandardRuntime.cpp
    runtime/ThunderboltRuntime.cpp
)
add_library(thunderbolt::thunderbolt ALIAS thunderbolt)

# Include root is the directory CONTAINING thunderbolt/, so public headers are
# reached as <thunderbolt/api/...>. Holds identically in both configure modes.
target_include_directories(thunderbolt PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
)

find_package(Threads REQUIRED)
target_link_libraries(thunderbolt PUBLIC thunderbolt::flags Threads::Threads)

target_compile_definitions(thunderbolt PRIVATE
    THUNDERBOLT_VERSION_STRING="${THUNDERBOLT_VERSION_STRING}"
    THUNDERBOLT_BUILD_CONFIG="$<CONFIG>"
)

if(WIN32)
    # Reserved for topology/affinity work in Phase C.
    target_link_libraries(thunderbolt PUBLIC kernel32)
endif()

set_target_properties(thunderbolt PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# ---------------------------------------------------------------------------
# Tests. Available in standalone mode too, so the runtime can be validated
# without any part of the game being present.
# ---------------------------------------------------------------------------
option(THUNDERBOLT_BUILD_TESTS "Build Thunderbolt HT unit tests" ON)
option(THUNDERBOLT_BUILD_EXAMPLES "Build Thunderbolt HT example programs" ON)
option(THUNDERBOLT_BUILD_BENCHMARKS "Build Thunderbolt HT benchmarks" ON)

if(THUNDERBOLT_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(THUNDERBOLT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(THUNDERBOLT_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()

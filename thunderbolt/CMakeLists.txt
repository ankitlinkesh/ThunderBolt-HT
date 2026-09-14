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
include(GNUInstallDirs)

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
# reached as <thunderbolt/api/...>. The install-interface path mirrors this: headers
# install to <prefix>/include/thunderbolt/..., so the include root there is just
# <prefix>/include.
target_include_directories(thunderbolt PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

# C++20 is a genuine usage requirement - public headers use concepts and requires
# clauses - so it is declared directly on this target rather than only inherited
# through thunderbolt::flags below, which does not survive into an installed package.
target_compile_features(thunderbolt PUBLIC cxx_std_20)

find_package(Threads REQUIRED)

# thunderbolt::flags carries -Werror/-WX and this project's own warning policy.
# That is exactly right for every in-tree consumer (engine/, game_benchmarks/,
# tests/ all link it explicitly themselves) and exactly wrong for an external
# package consumer, who should never have THIS project's warnings-as-errors
# policy silently imposed on THEIR code. $<BUILD_INTERFACE:...> keeps it applied
# while building thunderbolt itself and everything in this source tree, and drops
# it from the exported INSTALL_INTERFACE entirely.
target_link_libraries(thunderbolt PUBLIC $<BUILD_INTERFACE:thunderbolt::flags> Threads::Threads)

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

# ---------------------------------------------------------------------------
# Install / find_package(Thunderbolt) support.
#
# Off by default when consumed via add_subdirectory() by some OTHER project's
# build - that project's own `cmake --install` should not silently start
# installing Thunderbolt's targets into its tree unless it opts in. On by
# default for the repo's own top-level build and for the standalone configure
# (`cmake -S thunderbolt`), which is the actual packaging path.
# ---------------------------------------------------------------------------
option(THUNDERBOLT_INSTALL "Generate install rules and a find_package(Thunderbolt) config"
       "${THUNDERBOLT_STANDALONE}")

if(THUNDERBOLT_INSTALL)
    install(TARGETS thunderbolt
        EXPORT ThunderboltTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )

    # Everything the public headers transitively #include, not just the headers
    # an external consumer names directly - core/ and cpu/ are reached through
    # runtime/*.hpp even though nothing outside thunderbolt/ includes them by
    # name. benchmarks/ and tests/ are deliberately excluded: separate targets,
    # not part of what this library exports.
    install(DIRECTORY api core cpu runtime
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/thunderbolt"
        FILES_MATCHING PATTERN "*.hpp"
    )

    install(EXPORT ThunderboltTargets
        FILE ThunderboltTargets.cmake
        NAMESPACE thunderbolt::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Thunderbolt"
    )

    include(CMakePackageConfigHelpers)
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ThunderboltConfigVersion.cmake"
        VERSION "${THUNDERBOLT_VERSION_STRING}"
        COMPATIBILITY SameMajorVersion
    )
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThunderboltConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/ThunderboltConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Thunderbolt"
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ThunderboltConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ThunderboltConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Thunderbolt"
    )
endif()

# Thunderbolt HT benchmarks.
#
# A separate target from the library on purpose: the reference-runtime leg
# (oneTBB / Taskflow) may only ever be linked HERE, never into thunderbolt/, so
# that S3's "implement the scheduler ourselves" stays true while the numbers are
# still compared against an industrial implementation.

# ---------------------------------------------------------------------------
# External reference runtime.
#
# S11 asks for a competent baseline, and this project's own StandardRuntime is
# one - but "it beat my own baseline" is a weak claim. Comparing against an
# established industrial scheduler is what makes a speedup defensible.
#
# OFF by default, because a core build must never depend on the network. When
# enabled, Taskflow is pinned to a tag (S87 records what produced a result) and
# used HEADER-ONLY: its own CMake is skipped via a non-existent SOURCE_SUBDIR, so
# nothing of it is built, and its headers are SYSTEM-included so they do not have
# to survive this project's /W4 /WX.
#
# It is linked into THIS target only. Nothing under thunderbolt/ may reference it,
# which is what keeps S3's "implement the scheduler ourselves" true.
# ---------------------------------------------------------------------------
option(THUNDERBOLT_REFERENCE_RUNTIMES
       "Fetch Taskflow and include it as an external comparison leg" OFF)

set(THUNDERBOLT_HAVE_TASKFLOW OFF)
if(THUNDERBOLT_REFERENCE_RUNTIMES)
    include(FetchContent)
    FetchContent_Declare(taskflow
        GIT_REPOSITORY https://github.com/taskflow/taskflow.git
        GIT_TAG        v3.7.0
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  cmake-intentionally-absent
    )
    FetchContent_MakeAvailable(taskflow)
    if(EXISTS "${taskflow_SOURCE_DIR}/taskflow/taskflow.hpp")
        set(THUNDERBOLT_HAVE_TASKFLOW ON)
        # CACHE INTERNAL so game_benchmarks/ can see them too: the simulation needs
        # the same reference leg, and a directory-scoped variable would silently
        # leave that target without one.
        set(THUNDERBOLT_HAVE_TASKFLOW ON CACHE INTERNAL "Taskflow reference leg available")
        set(taskflow_SOURCE_DIR "${taskflow_SOURCE_DIR}" CACHE INTERNAL "Taskflow headers")
        message(STATUS "Reference runtime: Taskflow v3.7.0 at ${taskflow_SOURCE_DIR}")
    else()
        message(WARNING "Taskflow headers not found; the reference leg will be absent")
    endif()
endif()

# The measurement protocol, as a library.
#
# Extracted so the SIMULATION driver can use it too. It was previously private to
# this executable, and the result was a second measurement path in
# game_benchmarks/ that did single-shot timing with no repetitions, no
# interleaving and no throttle flagging - reporting numbers with 13-62% spread as
# if they were results. A harness that only one of two benchmark binaries uses is
# not a harness.
#
# It depends on thunderbolt/ only, never on engine/, so linking it into the
# simulation does not create a back-edge in the layering.
add_library(thunderbolt_bench_harness STATIC
    harness/BenchmarkRunner.cpp
    harness/Environment.cpp
    harness/ResultWriter.cpp
)
target_include_directories(thunderbolt_bench_harness PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(thunderbolt_bench_harness PUBLIC thunderbolt thunderbolt::flags)
set_target_properties(thunderbolt_bench_harness PROPERTIES CXX_EXTENSIONS OFF)

if(WIN32)
    # CallNtPowerInformation, for observing the actual clock frequency.
    target_link_libraries(thunderbolt_bench_harness PUBLIC PowrProf)
endif()

add_executable(thunderbolt_bench
    main.cpp
    experiments/Granularity.cpp
    experiments/Scaling.cpp
)

target_link_libraries(thunderbolt_bench PRIVATE
    thunderbolt_bench_harness thunderbolt thunderbolt::flags)
set_target_properties(thunderbolt_bench PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "thunderbolt-bench"
)

if(THUNDERBOLT_HAVE_TASKFLOW)
    target_include_directories(thunderbolt_bench SYSTEM PRIVATE "${taskflow_SOURCE_DIR}")
    target_compile_definitions(thunderbolt_bench PRIVATE
        THUNDERBOLT_HAVE_TASKFLOW=1
        TASKFLOW_VERSION_STRING="v3.7.0"
    )
endif()



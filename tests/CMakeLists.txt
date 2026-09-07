# Repository-level invariants.
#
# These do not test behaviour; they test that the ARCHITECTURE has not eroded.
# Both are cheap and both catch a class of mistake that unit tests cannot see.

# S4/S66: no file under thunderbolt/ may name engine/ or game/ headers, and no
# file under engine/ or game/ may reach past ITaskRuntime into runtime internals.
add_test(
    NAME architecture_layering
    COMMAND "${CMAKE_COMMAND}"
            -D "REPO_ROOT=${CMAKE_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_layering.cmake"
)

# S4: thunderbolt/ must configure and build with engine/ and game/ absent.
add_test(
    NAME thunderbolt_builds_standalone
    COMMAND "${CMAKE_COMMAND}"
            -D "REPO_ROOT=${CMAKE_SOURCE_DIR}"
            -D "SCRATCH=${CMAKE_BINARY_DIR}/standalone-check"
            -D "GENERATOR=${CMAKE_GENERATOR}"
            -D "GEN_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            -D "GEN_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_standalone.cmake"
)
set_tests_properties(thunderbolt_builds_standalone PROPERTIES TIMEOUT 600)

# The determinism harness. Substitutes for the ThreadSanitizer this toolchain
# does not have: identical world state from identical input under every schedule.
add_test(
    NAME simulation_determinism
    COMMAND "${CMAKE_COMMAND}"
            -D "SIM=$<TARGET_FILE:thunderbolt_sim>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_determinism.cmake"
)
set_tests_properties(simulation_determinism PROPERTIES TIMEOUT 900)

# Same 0xc0000135 hazard as thunderbolt_tests above, one directory up: under the
# ASan preset, thunderbolt-sim also links clang_rt.asan_dynamic-*.dll, and this
# test launches it from inside a CMake script (execute_process), which does not
# inherit a fix scoped to the thunderbolt_tests target. Without this the ONE
# check that substitutes for a missing ThreadSanitizer silently fails to run
# under the one preset built to catch races - exactly the kind of green suite
# that hides the bug that matters.
get_filename_component(THUNDERBOLT_MSVC_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
set_tests_properties(simulation_determinism PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${THUNDERBOLT_MSVC_BIN}"
)

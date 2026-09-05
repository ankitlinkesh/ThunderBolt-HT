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

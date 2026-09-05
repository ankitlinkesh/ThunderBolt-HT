# The determinism harness (S94, and the substitute for ThreadSanitizer).
#
# Runs the same scene and seed under every runtime and worker count, and requires
# a BIT-IDENTICAL world-state hash from all of them. A scheduler race that
# corrupts one float in one tick changes the hash; a test that only counted tasks
# would not notice.
#
# This is not hypothetical: it is what caught a lost dependency decrement that
# left tasks permanently Waiting, and which no unit test in the suite had found.

if(NOT DEFINED SIM)
    message(FATAL_ERROR "SIM (path to thunderbolt-sim) must be set")
endif()

set(scene "full_mixed")
set(seed 42)
set(ticks 40)

# Separated by "@" rather than ";": CMake flattens nested semicolon lists, so a
# list of "name;workers" pairs would arrive as one flat list of alternating
# strings.
set(configurations
    "serial@1"
    "standard@1"
    "standard@2"
    "standard@4"
    "standard@8"
    "thunderbolt@1"
    "thunderbolt@2"
    "thunderbolt@4"
    "thunderbolt@8"
)

set(reference "")
set(reference_name "")

foreach(configuration IN LISTS configurations)
    string(REPLACE "@" ";" parts "${configuration}")
    list(GET parts 0 runtime_name)
    list(GET parts 1 workers)

    execute_process(
        COMMAND "${SIM}" --scene ${scene} --seed ${seed} --ticks ${ticks}
                --runtime ${runtime_name} -w ${workers} --hash
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE  errors
        TIMEOUT 300
    )
    string(STRIP "${output}" hash)

    if(NOT result EQUAL 0)
        message(FATAL_ERROR
                "${runtime_name} w=${workers} failed (${result}).\n"
                "A non-zero exit here is usually a STALL, not a wrong answer.\n${errors}")
    endif()
    if(hash STREQUAL "")
        message(FATAL_ERROR "${runtime_name} w=${workers} produced no hash.\n${errors}")
    endif()

    if(reference STREQUAL "")
        set(reference "${hash}")
        set(reference_name "${runtime_name} w=${workers}")
        message(STATUS "reference: ${reference_name} -> ${hash}")
    elseif(NOT hash STREQUAL reference)
        message(FATAL_ERROR
                "DETERMINISM VIOLATED.\n"
                "  ${reference_name} -> ${reference}\n"
                "  ${runtime_name} w=${workers} -> ${hash}\n"
                "The same seed produced different world state under a different "
                "schedule. That is a data race, or a reduction whose partitioning "
                "depends on worker count.")
    else()
        message(STATUS "match: ${runtime_name} w=${workers}")
    endif()
endforeach()

# A hash that never changes would pass the checks above while proving nothing, so
# confirm the hash actually responds to the simulation.
execute_process(
    COMMAND "${SIM}" --scene ${scene} --seed 43 --ticks ${ticks}
            --runtime thunderbolt -w 8 --hash
    OUTPUT_VARIABLE other_output
    TIMEOUT 300
)
string(STRIP "${other_output}" other_hash)
if(other_hash STREQUAL reference)
    message(FATAL_ERROR
            "A different seed produced the same hash. The hash is not sensitive to "
            "simulation state and the whole harness is worthless.")
endif()

message(STATUS "determinism OK across ${scene}: all runtimes and worker counts agree")

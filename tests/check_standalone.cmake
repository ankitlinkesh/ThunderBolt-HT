# Standalone-buildability guard (S4).
#
# "Thunderbolt HT can be built independently" is a design requirement, and the
# only honest way to test it is to configure and build the runtime with engine/
# and game/ genuinely ABSENT - not merely unreferenced. So: copy thunderbolt/
# into a scratch tree that contains nothing else, and build it there.

if(NOT DEFINED REPO_ROOT OR NOT DEFINED SCRATCH)
    message(FATAL_ERROR "REPO_ROOT and SCRATCH must be set")
endif()

# CMAKE_GENERATOR is NOT populated in -P script mode, so the parent build passes
# its own generator selection in. Mirroring platform and toolset matters as much
# as the generator itself: the point of the test is that the runtime builds under
# the SAME pinned toolset the rest of the project uses (S87).
if(NOT DEFINED GENERATOR OR GENERATOR STREQUAL "")
    message(FATAL_ERROR "GENERATOR must be passed in; it is empty in script mode")
endif()

set(gen_args -G "${GENERATOR}")
if(DEFINED GEN_PLATFORM AND NOT GEN_PLATFORM STREQUAL "")
    list(APPEND gen_args -A "${GEN_PLATFORM}")
endif()
if(DEFINED GEN_TOOLSET AND NOT GEN_TOOLSET STREQUAL "")
    list(APPEND gen_args -T "${GEN_TOOLSET}")
endif()

set(src_copy "${SCRATCH}/thunderbolt")
set(build_dir "${SCRATCH}/build")

file(REMOVE_RECURSE "${SCRATCH}")
file(MAKE_DIRECTORY "${SCRATCH}")

file(COPY "${REPO_ROOT}/thunderbolt" DESTINATION "${SCRATCH}")

if(EXISTS "${SCRATCH}/engine" OR EXISTS "${SCRATCH}/game")
    message(FATAL_ERROR "scratch tree is contaminated; the test would prove nothing")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${src_copy}" -B "${build_dir}"
            ${gen_args}
            -D CMAKE_BUILD_TYPE=Release
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE  configure_output
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "standalone configure failed:\n${configure_output}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${build_dir}" --config Release
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE  build_output
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "standalone build failed:\n${build_output}")
endif()

message(STATUS "Thunderbolt HT builds standalone with engine/ and game/ absent")

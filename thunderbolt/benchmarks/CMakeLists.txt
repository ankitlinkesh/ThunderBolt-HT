# Thunderbolt HT benchmarks.
#
# A separate target from the library on purpose: the reference-runtime leg
# (oneTBB / Taskflow) may only ever be linked HERE, never into thunderbolt/, so
# that S3's "implement the scheduler ourselves" stays true while the numbers are
# still compared against an industrial implementation.

add_executable(thunderbolt_bench
    main.cpp
    harness/BenchmarkRunner.cpp
    harness/Environment.cpp
    harness/ResultWriter.cpp
    experiments/Granularity.cpp
    experiments/Scaling.cpp
)

target_link_libraries(thunderbolt_bench PRIVATE thunderbolt thunderbolt::flags)
set_target_properties(thunderbolt_bench PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "thunderbolt-bench"
)

if(WIN32)
    # CallNtPowerInformation, used to observe the actual clock frequency so that
    # thermally throttled samples are visible rather than silently averaged in.
    target_link_libraries(thunderbolt_bench PRIVATE PowrProf)
endif()

# The headless simulation driver.
#
# This is where S94's demonstration lives: the same workload, the same seed, run
# under either runtime, producing an identical state hash and different timings.
add_executable(thunderbolt_sim
    main.cpp
    TaskflowSim.cpp
)
target_link_libraries(thunderbolt_sim PRIVATE tbworld thunderbolt thunderbolt::flags)
set_target_properties(thunderbolt_sim PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "thunderbolt-sim"
)

# The external reference leg. Fetched and configured by thunderbolt/benchmarks/,
# and used here ONLY - nothing under thunderbolt/ or engine/ may reference it, so
# S3's "implement the scheduler ourselves" stays true while the strongest claim in
# the project is still checked against an industrial scheduler.
if(THUNDERBOLT_HAVE_TASKFLOW)
    target_include_directories(thunderbolt_sim SYSTEM PRIVATE "${taskflow_SOURCE_DIR}")
    target_compile_definitions(thunderbolt_sim PRIVATE
        THUNDERBOLT_HAVE_TASKFLOW=1
        TASKFLOW_VERSION_STRING="v3.7.0"
    )
endif()

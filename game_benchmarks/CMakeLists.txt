# The headless simulation driver.
#
# This is where S94's demonstration lives: the same workload, the same seed, run
# under either runtime, producing an identical state hash and different timings.
add_executable(thunderbolt_sim main.cpp)
target_link_libraries(thunderbolt_sim PRIVATE tbworld thunderbolt thunderbolt::flags)
set_target_properties(thunderbolt_sim PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "thunderbolt-sim"
)

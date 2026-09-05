# Thunderbolt Open World - the simulation.
#
# Depends on thunderbolt/; the reverse is forbidden and enforced by the
# architecture_layering test. This is the workload, not the runtime.
add_library(tbworld STATIC
    core/World.cpp
    core/Systems.cpp
    core/Simulation.cpp
)

target_include_directories(tbworld PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>")
target_link_libraries(tbworld PUBLIC thunderbolt thunderbolt::flags)
set_target_properties(tbworld PROPERTIES CXX_EXTENSIONS OFF)

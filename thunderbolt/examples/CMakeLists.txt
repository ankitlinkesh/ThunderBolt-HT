# Small programs that exercise the runtime on real hardware.
#
# These are not tests: they exist for the checks a test cannot honestly make,
# such as confirming that CPU topology detection really queried the OS rather
# than quietly taking its fallback path.

add_executable(thunderbolt_topology_report topology_report.cpp)
target_link_libraries(thunderbolt_topology_report PRIVATE thunderbolt thunderbolt::flags)
set_target_properties(thunderbolt_topology_report PROPERTIES CXX_EXTENSIONS OFF)

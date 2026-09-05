# Thunderbolt HT unit tests.
#
# No external test framework yet: a dependency-free assertion harness keeps the
# standalone-build requirement (S4) trivially true and avoids fighting /W4 /WX
# over third-party headers. Revisit if the suite outgrows it.

add_executable(thunderbolt_tests
    main.cpp
    test_build_info.cpp
    test_task_function.cpp
    test_task_pool.cpp
    test_abp_deque.cpp
    test_cpu_topology.cpp
    test_runtime_conformance.cpp
    test_dependency_conformance.cpp
    test_task_graph.cpp
    test_json_writer.cpp
    test_scheduler_modes.cpp
    test_thunderbolt_runtime.cpp
)

target_link_libraries(thunderbolt_tests PRIVATE thunderbolt thunderbolt::flags)
set_target_properties(thunderbolt_tests PROPERTIES CXX_EXTENSIONS OFF)

add_test(NAME thunderbolt_tests COMMAND thunderbolt_tests)

# Under the ASan preset the test binary needs clang_rt.asan_dynamic-*.dll, which
# lives beside cl.exe and is not on PATH. Without this the suite fails at load
# with 0xc0000135 (DLL not found) and looks like a broken test rather than a
# missing runtime. Prepending the compiler directory is harmless for non-ASan
# configurations, so it is applied unconditionally.
get_filename_component(THUNDERBOLT_MSVC_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
set_tests_properties(thunderbolt_tests PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${THUNDERBOLT_MSVC_BIN}"
)

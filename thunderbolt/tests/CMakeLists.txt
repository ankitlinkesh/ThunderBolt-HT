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

# tsan.supp suppresses one specific, reasoned-through pattern in AbpDeque (see
# that file's own header comment and its steal()/push() bodies): the deque's
# buffer-slot access is ordered entirely by the top_/bottom_ index protocol,
# not by the access itself, and push()'s capacity guard makes it algorithmically
# impossible for a thief's read index and the owner's write index to ever alias
# the same physical slot - an invariant TSan's plain happens-before tracking
# cannot see. This suppresses that one class, not the file generally; see
# tsan.supp for the full argument. Setting TSAN_OPTIONS is a no-op under every
# other sanitizer/config (only the ThreadSanitizer runtime reads it), so it is
# applied unconditionally rather than gated on the tsan preset.
set_tests_properties(thunderbolt_tests PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${THUNDERBOLT_MSVC_BIN};TSAN_OPTIONS=set:suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp"
)

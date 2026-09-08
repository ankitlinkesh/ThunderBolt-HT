# Thunderbolt HT - shared compiler configuration.
#
# Lives inside thunderbolt/ (not the repo-root cmake/) so that thunderbolt/ stays
# configurable as a standalone project with no engine/ or game/ present. See PLAN
# "Runtime independence (S4/S66)".
#
# The floating-point settings here are NOT stylistic. The determinism harness
# requires a bit-identical world-state hash across runtimes, worker counts and
# build configurations; that only holds if FP evaluation order and contraction
# are pinned identically everywhere. Do not add /fp:fast or per-config /arch
# flags without reading the PLAN section "Determinism is the correctness proof".

include_guard(GLOBAL)

if(TARGET thunderbolt_flags)
    return()
endif()

add_library(thunderbolt_flags INTERFACE)
add_library(thunderbolt::flags ALIAS thunderbolt_flags)

target_compile_features(thunderbolt_flags INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(thunderbolt_flags INTERFACE
        /W4                 # high warning level
        /WX                 # warnings are errors (third-party deps must be SYSTEM includes)
        /permissive-        # conformance mode
        /Zc:preprocessor    # conforming preprocessor
        /Zc:__cplusplus     # report the real __cplusplus value
        /EHsc               # standard C++ exception model
        /utf-8              # source and execution charset
        /fp:precise         # pinned FP semantics; also disables contraction by default
        /volatile:iso       # volatile is not an implicit memory fence - forces real atomics
        /MP                 # parallel compilation
    )
    # Deliberately NOT set: /arch:AVX2 and friends. Leaving the x64 SSE2 baseline
    # keeps codegen consistent across configurations, which the determinism hash
    # depends on. Revisit only with a measurement and a re-run of the hash tests.
    target_compile_definitions(thunderbolt_flags INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(thunderbolt_flags INTERFACE
        -Wall -Wextra -Wpedantic -Werror
        -ffp-contract=off   # GCC/Clang equivalent of the MSVC contraction pin
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # CMAKE_CXX_EXTENSIONS OFF (set project-wide) means -std=c++20 rather
        # than -std=gnu++20, and GCC/Clang only auto-define _GNU_SOURCE in the
        # gnu++ dialect. Without it explicitly, glibc's <pthread.h> and
        # <sched.h> hide pthread_setaffinity_np and the CPU_SET/CPU_ZERO
        # macros behind #ifdef __USE_GNU, which Affinity.cpp needs. Applied as
        # an INTERFACE definition here (compiles to -D_GNU_SOURCE on every
        # consumer's command line) rather than a #define in that file, because
        # it must be visible before the FIRST system header anywhere in the
        # translation unit is processed - a #define after any earlier #include
        # (even of another Thunderbolt header that pulls in <vector>) can be
        # too late once glibc's feature-test-macro cascade has already run.
        target_compile_definitions(thunderbolt_flags INTERFACE _GNU_SOURCE)
    endif()
endif()

# Debug-build assertions (S62: "in debug builds, fail loudly").
target_compile_definitions(thunderbolt_flags INTERFACE
    $<$<CONFIG:Debug>:THUNDERBOLT_DEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:THUNDERBOLT_DEBUG=0>
)

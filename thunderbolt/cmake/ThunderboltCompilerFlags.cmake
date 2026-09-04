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
endif()

# Debug-build assertions (S62: "in debug builds, fail loudly").
target_compile_definitions(thunderbolt_flags INTERFACE
    $<$<CONFIG:Debug>:THUNDERBOLT_DEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:THUNDERBOLT_DEBUG=0>
)

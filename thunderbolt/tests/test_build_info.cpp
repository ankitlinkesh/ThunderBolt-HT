#include "TestHarness.hpp"

#include <thunderbolt/api/BuildInfo.hpp>

// S87 requires benchmark output to record the exact build. If any of these fields
// is empty the reproducibility block in a results JSON would be silently useless,
// so assert they are actually populated rather than merely present.
TB_TEST("build_info reports non-empty provenance") {
    const auto& info = thunderbolt::build_info();
    TB_CHECK(!info.version.empty());
    TB_CHECK(!info.compiler.empty());
    TB_CHECK(!info.configuration.empty());
    TB_CHECK(!info.cpp_standard.empty());
}

TB_TEST("build_info reports C++20 or newer") {
    // Guards against the /Zc:__cplusplus flag being dropped from the flags module,
    // which would silently report 199711L under MSVC.
    TB_CHECK(__cplusplus >= 202002L);
}

TB_TEST("debug assertions flag matches build configuration") {
    const auto& info = thunderbolt::build_info();
#if THUNDERBOLT_DEBUG
    TB_CHECK(info.debug_assertions);
#else
    TB_CHECK(!info.debug_assertions);
#endif
}

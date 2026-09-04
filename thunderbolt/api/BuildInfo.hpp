// Thunderbolt HT - build provenance.
//
// S87 requires every benchmark result to record the exact build that produced it.
// Capturing that at compile time here means a results JSON can never disagree with
// the binary that wrote it. Two MSVC toolsets are installed on the reference
// machine, so "which compiler" is a real question, not a formality.
#pragma once

#include <string_view>

namespace thunderbolt {

struct BuildInfo {
    std::string_view version;         // Thunderbolt HT version
    std::string_view compiler;        // compiler id and version
    std::string_view configuration;   // Debug / Release / RelWithDebInfo / ASan
    std::string_view cpp_standard;    // __cplusplus as reported by the compiler
    bool             debug_assertions; // THUNDERBOLT_DEBUG
    bool             address_sanitizer;
};

// Provenance of the translation unit that compiled BuildInfo.cpp.
[[nodiscard]] const BuildInfo& build_info() noexcept;

} // namespace thunderbolt

// Thunderbolt HT - what produced a benchmark result.
//
// S87 lists the fields a reproducible result must carry. The reason is not
// bookkeeping: a speedup number is meaningless without the machine, the build and
// the configuration that produced it, and a results file that omits them cannot
// be checked by anyone later - including its author.
//
// The clock sampler is here for a machine-specific reason. The reference CPU is a
// 15 W part whose sustained frequency drops under exactly the load this project
// generates, and that drift is larger than the effects being measured. Sampling
// the frequency around each run makes a throttled run VISIBLE rather than
// silently averaged into the result.
#pragma once

#include <cstdint>
#include <string>

namespace thunderbolt::bench {

struct Environment {
    // Hardware
    std::string   cpu_brand;
    std::uint32_t physical_cores     = 0;  // 0 means "not detected" - never guessed
    std::uint32_t logical_processors = 0;
    bool          smt                = false;
    std::uint64_t ram_bytes          = 0;

    // Software
    std::string os;
    std::string compiler;
    std::string build_config;
    std::string thunderbolt_version;
    bool        debug_assertions  = false;
    bool        address_sanitizer = false;

    // When
    std::string timestamp_utc;
};

[[nodiscard]] Environment capture_environment();

// Average current clock across logical processors, in MHz. Returns 0 when the
// platform cannot report it - in which case results must not claim the run was
// un-throttled, only that frequency was unavailable.
[[nodiscard]] double sample_current_mhz();

// Nominal maximum clock in MHz, or 0 when unavailable.
[[nodiscard]] double nominal_max_mhz();

} // namespace thunderbolt::bench

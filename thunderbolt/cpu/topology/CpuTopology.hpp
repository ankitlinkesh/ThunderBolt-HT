// Thunderbolt HT - CPU topology discovery.
//
// S16 is blunt about the reason: "Do not assume logical processors are equivalent
// to physical cores." On the reference machine the difference is 4 versus 8, and
// a scaling sweep that does not distinguish them reports an efficiency cliff past
// 4 workers that looks like a scheduler defect and is actually SMT. Every scaling
// result must be able to label which is which, so this is a reporting requirement
// before it is a scheduling one.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace thunderbolt {

struct CoreInfo {
    // Indices of the logical processors belonging to this physical core.
    // More than one means SMT: those siblings share execution resources, so two
    // workers on them do NOT deliver two cores' worth of throughput.
    std::vector<std::uint32_t> logical_processors;

    [[nodiscard]] bool is_smt() const noexcept { return logical_processors.size() > 1; }
};

struct CpuTopology {
    std::uint32_t logical_processor_count = 0;
    std::uint32_t physical_core_count     = 0;
    std::uint32_t package_count           = 0;

    std::vector<CoreInfo> cores;

    // Best-effort brand string; empty when unavailable. Recorded in results (S87).
    std::string brand;

    // True when discovery actually queried the OS. False means the values are a
    // std::thread::hardware_concurrency fallback, in which case physical core
    // count is NOT trustworthy and results must not claim otherwise.
    bool detected = false;

    [[nodiscard]] bool has_smt() const noexcept {
        return detected && physical_core_count > 0 &&
               logical_processor_count > physical_core_count;
    }
};

// Queries the platform once and caches the result.
[[nodiscard]] const CpuTopology& cpu_topology();

// Human-readable summary for logs and benchmark headers.
[[nodiscard]] std::string describe_topology(const CpuTopology& topology);

} // namespace thunderbolt

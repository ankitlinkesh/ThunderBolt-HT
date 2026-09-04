#include <thunderbolt/cpu/affinity/Affinity.hpp>

#include <thunderbolt/cpu/topology/CpuTopology.hpp>

#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

namespace thunderbolt {
namespace {

// Orders logical processors so that consecutive workers land on DIFFERENT
// physical cores first, and only start sharing SMT siblings once every core is
// occupied.
//
// The naive ordering - logical 0, 1, 2, 3 - puts workers 0 and 1 on the two
// hyperthreads of core 0 on a typical Intel layout, so a two-worker run shares
// one core's execution resources and measures roughly half the throughput it
// should. That is a mistake that looks like poor scaling, which is exactly the
// conclusion the scaling sweep is supposed to draw honestly.
std::vector<std::uint32_t> spread_order(const CpuTopology& topology) {
    std::vector<std::uint32_t> order;
    if (!topology.detected || topology.cores.empty()) {
        return order;
    }

    order.reserve(topology.logical_processor_count);

    std::size_t max_siblings = 0;
    for (const CoreInfo& core : topology.cores) {
        max_siblings = (core.logical_processors.size() > max_siblings)
                           ? core.logical_processors.size()
                           : max_siblings;
    }

    // Round `sibling`: take the Nth hyperthread of every core before moving on.
    for (std::size_t sibling = 0; sibling < max_siblings; ++sibling) {
        for (const CoreInfo& core : topology.cores) {
            if (sibling < core.logical_processors.size()) {
                order.push_back(core.logical_processors[sibling]);
            }
        }
    }
    return order;
}

} // namespace

bool apply_worker_affinity(std::uint32_t worker_index, AffinityMode mode) {
    if (mode == AffinityMode::Disabled) {
        return false;
    }

    const CpuTopology& topology = cpu_topology();
    const std::vector<std::uint32_t> order = spread_order(topology);
    if (order.empty()) {
        return false;  // topology unknown: refuse to guess at a mapping
    }

    const std::uint32_t logical = order[worker_index % order.size()];

#if defined(_WIN32)
    // Single-group only. Machines with more than 64 logical processors need
    // SetThreadGroupAffinity; refusing is better than silently pinning to the
    // wrong group.
    if (logical >= 64) {
        return false;
    }
    const DWORD_PTR mask   = static_cast<DWORD_PTR>(1ull) << logical;
    const DWORD_PTR result = SetThreadAffinityMask(GetCurrentThread(), mask);
    return result != 0;
#else
    (void)logical;
    return false;
#endif
}

} // namespace thunderbolt

#include "TestHarness.hpp"

#include <thunderbolt/cpu/topology/CpuTopology.hpp>

#include <thread>

TB_TEST("topology reports a plausible logical processor count") {
    const auto& topology = thunderbolt::cpu_topology();
    TB_CHECK(topology.logical_processor_count >= 1);
    TB_CHECK(topology.logical_processor_count == std::thread::hardware_concurrency() ||
             std::thread::hardware_concurrency() == 0);
}

TB_TEST("physical cores are reported as unknown rather than guessed") {
    // S16 exists because logical processors are not cores. If detection fails,
    // the honest answer is 0/"unknown" - guessing logical/2 would silently
    // mislabel every scaling result on a machine where that ratio is wrong.
    const auto& topology = thunderbolt::cpu_topology();
    if (!topology.detected) {
        TB_CHECK_EQ(topology.physical_core_count, 0u);
        return;
    }
    TB_CHECK(topology.physical_core_count >= 1);
    TB_CHECK(topology.physical_core_count <= topology.logical_processor_count);
}

TB_TEST("core sibling lists account for every logical processor") {
    const auto& topology = thunderbolt::cpu_topology();
    if (!topology.detected) {
        return;
    }
    std::uint32_t total = 0;
    for (const auto& core : topology.cores) {
        TB_CHECK(!core.logical_processors.empty());
        total += static_cast<std::uint32_t>(core.logical_processors.size());
    }
    TB_CHECK_EQ(total, topology.logical_processor_count);
}

TB_TEST("SMT is reported only when siblings actually exist") {
    const auto& topology = thunderbolt::cpu_topology();
    if (!topology.detected) {
        TB_CHECK(!topology.has_smt());
        return;
    }
    bool any_smt_core = false;
    for (const auto& core : topology.cores) {
        any_smt_core = any_smt_core || core.is_smt();
    }
    TB_CHECK_EQ(topology.has_smt(), any_smt_core);
}

TB_TEST("topology description is non-empty and states core count honestly") {
    const auto& topology = thunderbolt::cpu_topology();
    const std::string text = thunderbolt::describe_topology(topology);
    TB_CHECK(!text.empty());
    TB_CHECK(text.find("logical processors:") != std::string::npos);
    if (!topology.detected) {
        TB_CHECK(text.find("unknown") != std::string::npos);
    }
}

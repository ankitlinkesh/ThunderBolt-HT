// Prints what the runtime actually detected about this machine.
//
// Exists because the topology unit tests deliberately tolerate the fallback path
// (a machine where the OS query is unavailable is not a test failure) - which
// means detection could be silently broken and the suite would stay green. This
// is the check that a human runs once on real hardware to confirm the numbers are
// real rather than guessed. Every benchmark result depends on them.
#include <thunderbolt/cpu/topology/CpuTopology.hpp>
#include <thunderbolt/api/BuildInfo.hpp>

#include <cstdio>
#include <string>

int main() {
    const auto& info = thunderbolt::build_info();
    std::printf("Thunderbolt HT %.*s  [%.*s, %.*s]\n\n",
                static_cast<int>(info.version.size()), info.version.data(),
                static_cast<int>(info.compiler.size()), info.compiler.data(),
                static_cast<int>(info.configuration.size()), info.configuration.data());

    const auto& topology = thunderbolt::cpu_topology();
    const std::string text = thunderbolt::describe_topology(topology);
    std::fputs(text.c_str(), stdout);

    std::printf("\ndetection: %s\n", topology.detected ? "OS query succeeded"
                                                       : "FELL BACK (core count unknown)");

    if (topology.detected) {
        std::printf("\nper-core logical processors:\n");
        for (std::size_t i = 0; i < topology.cores.size(); ++i) {
            std::printf("  core %2zu:", i);
            for (std::uint32_t lp : topology.cores[i].logical_processors) {
                std::printf(" %u", lp);
            }
            std::printf("%s\n", topology.cores[i].is_smt() ? "   (SMT siblings)" : "");
        }
    }
    return 0;
}

#include <thunderbolt/cpu/topology/CpuTopology.hpp>

#include <array>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#  include <windows.h>
#endif

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

namespace thunderbolt {
namespace {

std::string query_brand() {
#if defined(_MSC_VER)
    // CPUID leaves 0x80000002..4 hold the 48-byte brand string, when supported.
    std::array<int, 4> regs{};
    __cpuid(regs.data(), 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004u) {
        return {};
    }

    char brand[49] = {};
    for (unsigned leaf = 0; leaf < 3; ++leaf) {
        __cpuid(regs.data(), static_cast<int>(0x80000002u + leaf));
        std::memcpy(brand + leaf * 16, regs.data(), 16);
    }

    std::string result(brand);
    // Intel pads the brand string with leading spaces.
    const std::size_t first = result.find_first_not_of(' ');
    const std::size_t last  = result.find_last_not_of(" \t\0");
    if (first == std::string::npos) {
        return {};
    }
    return result.substr(first, last - first + 1);
#else
    return {};
#endif
}

#if defined(_WIN32)

bool query_windows_topology(CpuTopology& out) {
    // GetLogicalProcessorInformationEx is the only API that reports the
    // core -> logical-processor mapping. GetSystemInfo gives logical count only,
    // which is exactly the number that must not be mistaken for cores.
    DWORD length = 0;
    if (GetLogicalProcessorInformationEx(RelationAll, nullptr, &length) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return false;
    }

    std::vector<std::uint8_t> buffer(length);
    if (!GetLogicalProcessorInformationEx(
            RelationAll,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length)) {
        return false;
    }

    std::uint32_t highest_logical = 0;

    DWORD offset = 0;
    while (offset < length) {
        auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() +
                                                                                offset);
        if (info->Size == 0) {
            break;  // malformed; refuse to loop forever
        }

        switch (info->Relationship) {
        case RelationProcessorCore: {
            CoreInfo core;
            // A core may span several GROUP_AFFINITY entries on machines with
            // more than 64 logical processors.
            for (WORD g = 0; g < info->Processor.GroupCount; ++g) {
                const GROUP_AFFINITY& group = info->Processor.GroupMask[g];
                for (std::uint32_t bit = 0; bit < sizeof(KAFFINITY) * 8; ++bit) {
                    if ((group.Mask >> bit) & 1u) {
                        const auto logical =
                            static_cast<std::uint32_t>(group.Group) * 64u + bit;
                        core.logical_processors.push_back(logical);
                        highest_logical = (logical + 1 > highest_logical) ? logical + 1
                                                                         : highest_logical;
                    }
                }
            }
            if (!core.logical_processors.empty()) {
                out.cores.push_back(std::move(core));
            }
            break;
        }
        case RelationProcessorPackage:
            ++out.package_count;
            break;
        default:
            break;
        }

        offset += info->Size;
    }

    if (out.cores.empty()) {
        return false;
    }

    out.physical_core_count = static_cast<std::uint32_t>(out.cores.size());

    std::uint32_t logical_total = 0;
    for (const CoreInfo& core : out.cores) {
        logical_total += static_cast<std::uint32_t>(core.logical_processors.size());
    }
    out.logical_processor_count = logical_total;

    if (out.package_count == 0) {
        out.package_count = 1;
    }
    return true;
}

#endif // _WIN32

CpuTopology detect() {
    CpuTopology topology;
    topology.brand = query_brand();

#if defined(_WIN32)
    topology.detected = query_windows_topology(topology);
#endif

    if (!topology.detected) {
        // Fallback. Note what is NOT claimed here: physical_core_count stays 0
        // rather than being guessed at as logical/2, because a wrong core count
        // silently mislabels every scaling result. Reporting "unknown" is the
        // honest answer and callers check `detected`.
        const unsigned hw = std::thread::hardware_concurrency();
        topology.logical_processor_count = hw > 0 ? hw : 1;
        topology.physical_core_count     = 0;
        topology.package_count           = 0;
    }

    return topology;
}

} // namespace

const CpuTopology& cpu_topology() {
    // Function-local static: computed once, thread-safe initialisation, and no
    // static initialisation order dependency.
    static const CpuTopology topology = detect();
    return topology;
}

std::string describe_topology(const CpuTopology& topology) {
    std::string out;
    if (!topology.brand.empty()) {
        out += topology.brand;
        out += "\n";
    }

    out += "logical processors: " + std::to_string(topology.logical_processor_count) + "\n";

    if (!topology.detected || topology.physical_core_count == 0) {
        out += "physical cores: unknown (topology query unavailable)\n";
        return out;
    }

    out += "physical cores: " + std::to_string(topology.physical_core_count) + "\n";
    out += "packages: " + std::to_string(topology.package_count) + "\n";
    out += topology.has_smt() ? "SMT: yes\n" : "SMT: no\n";
    return out;
}

} // namespace thunderbolt

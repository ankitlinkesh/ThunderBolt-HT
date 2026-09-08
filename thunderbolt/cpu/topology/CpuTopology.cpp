#include <thunderbolt/cpu/topology/CpuTopology.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#  include <windows.h>
#endif

#if defined(_MSC_VER)
#  include <intrin.h>
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
// GCC and Clang both provide this header on x86; it wraps the CPUID
// instruction the same way <intrin.h> does for MSVC.
#  include <cpuid.h>
#endif

#if defined(__linux__)
#  include <cctype>
#  include <dirent.h>
#  include <fstream>
#  include <map>
#  include <set>
#  include <utility>
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
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
    // Same CPUID leaves, read through GCC/Clang's __cpuid macro instead of an
    // MSVC intrinsic. __cpuid(level, a, b, c, d) writes EAX/EBX/ECX/EDX after
    // executing CPUID with EAX=level into the four output arguments.
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    __cpuid(0x80000000u, eax, ebx, ecx, edx);
    if (eax < 0x80000004u) {
        return {};
    }

    char brand[49] = {};
    for (unsigned leaf = 0; leaf < 3; ++leaf) {
        __cpuid(0x80000002u + leaf, eax, ebx, ecx, edx);
        // x86 is little-endian, so the register value's bytes are the brand
        // string's bytes in order - the same reasoning MSVC's __cpuid path
        // relies on for its single memcpy of all four registers.
        std::memcpy(brand + leaf * 16 + 0, &eax, 4);
        std::memcpy(brand + leaf * 16 + 4, &ebx, 4);
        std::memcpy(brand + leaf * 16 + 8, &ecx, 4);
        std::memcpy(brand + leaf * 16 + 12, &edx, 4);
    }

    std::string result(brand);
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

#if defined(__linux__)

// Reads a single unsigned integer from a sysfs file. Returns false (leaving
// `value` untouched) for a CPU that is offline or a kernel too old to expose
// the file - both are normal, not errors, and the caller treats that logical
// processor as simply unavailable to report on rather than failing outright.
bool read_uint_file(const std::string& path, std::uint32_t& value) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    file >> value;
    return static_cast<bool>(file);
}

bool query_linux_topology(CpuTopology& out) {
    // /sys/devices/system/cpu/cpuN/topology/{core_id,physical_package_id} is
    // the same information GetLogicalProcessorInformationEx reports on
    // Windows: which logical processors share a physical core (SMT siblings),
    // and which package they belong to. Enumerated via the directory itself
    // rather than a CPU count, because a range like "0-3,8-11" (some CPUs
    // offline) is not simply "0..N".
    DIR* dir = opendir("/sys/devices/system/cpu");
    if (dir == nullptr) {
        return false;
    }

    std::vector<std::uint32_t> logical_ids;
    for (dirent* entry = readdir(dir); entry != nullptr; entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() <= 3 || name.compare(0, 3, "cpu") != 0) {
            continue;
        }
        bool all_digits = true;
        for (std::size_t i = 3; i < name.size(); ++i) {
            if (std::isdigit(static_cast<unsigned char>(name[i])) == 0) {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            logical_ids.push_back(static_cast<std::uint32_t>(std::stoul(name.substr(3))));
        }
    }
    closedir(dir);

    if (logical_ids.empty()) {
        return false;
    }

    // (package_id, core_id) uniquely identifies a physical core across the
    // whole machine; core_id alone repeats per package.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> core_index;
    std::set<std::uint32_t>                                        package_ids;

    for (std::uint32_t logical : logical_ids) {
        const std::string base =
            "/sys/devices/system/cpu/cpu" + std::to_string(logical) + "/topology/";
        std::uint32_t package_id = 0;
        std::uint32_t core_id    = 0;
        if (!read_uint_file(base + "physical_package_id", package_id) ||
            !read_uint_file(base + "core_id", core_id)) {
            continue;  // offline, or a kernel that does not expose topology/
        }
        package_ids.insert(package_id);

        const auto key = std::make_pair(package_id, core_id);
        const auto it  = core_index.find(key);
        if (it == core_index.end()) {
            core_index.emplace(key, out.cores.size());
            CoreInfo core;
            core.logical_processors.push_back(logical);
            out.cores.push_back(std::move(core));
        } else {
            out.cores[it->second].logical_processors.push_back(logical);
        }
    }

    if (out.cores.empty()) {
        return false;
    }

    // Sorted for a stable, readable report - not load-bearing for correctness,
    // since spread_order() in Affinity.cpp reads logical_processors regardless
    // of order.
    for (CoreInfo& core : out.cores) {
        std::sort(core.logical_processors.begin(), core.logical_processors.end());
    }
    std::sort(out.cores.begin(), out.cores.end(), [](const CoreInfo& a, const CoreInfo& b) {
        return a.logical_processors.front() < b.logical_processors.front();
    });

    out.physical_core_count = static_cast<std::uint32_t>(out.cores.size());

    std::uint32_t logical_total = 0;
    for (const CoreInfo& core : out.cores) {
        logical_total += static_cast<std::uint32_t>(core.logical_processors.size());
    }
    out.logical_processor_count = logical_total;

    out.package_count = static_cast<std::uint32_t>(package_ids.size());
    if (out.package_count == 0) {
        out.package_count = 1;
    }
    return true;
}

#endif // __linux__

CpuTopology detect() {
    CpuTopology topology;
    topology.brand = query_brand();

#if defined(_WIN32)
    topology.detected = query_windows_topology(topology);
#elif defined(__linux__)
    topology.detected = query_linux_topology(topology);
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

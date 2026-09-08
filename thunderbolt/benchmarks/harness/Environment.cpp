#include "Environment.hpp"

#include <thunderbolt/api/BuildInfo.hpp>
#include <thunderbolt/cpu/topology/CpuTopology.hpp>

#include <chrono>
#include <ctime>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
// powerbase.h must follow windows.h.
#  include <powerbase.h>
#elif defined(__linux__)
#  include <cstdio>
#  include <fstream>
#  include <sstream>
#  include <sys/sysinfo.h>
#  include <sys/utsname.h>
#endif

namespace thunderbolt::bench {
namespace {

#if defined(_WIN32)

// CallNtPowerInformation(ProcessorInformation) fills one of these per logical
// processor. It is the only documented way to read the CURRENT clock without a
// driver, which matters because a nominal frequency says nothing about what the
// chip was actually doing while throttled.
struct ProcessorPowerInformation {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

bool query_processor_power(std::vector<ProcessorPowerInformation>& out) {
    const std::uint32_t count = cpu_topology().logical_processor_count;
    if (count == 0) {
        return false;
    }
    out.resize(count);
    const LONG status = CallNtPowerInformation(
        ProcessorInformation, nullptr, 0, out.data(),
        static_cast<ULONG>(out.size() * sizeof(ProcessorPowerInformation)));
    return status == 0;  // STATUS_SUCCESS
}

std::string query_os() {
    // RtlGetVersion reports the real version; GetVersionEx lies for compatibility
    // unless the binary carries a manifest, which would make the recorded OS
    // wrong in exactly the field meant to identify the machine.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll != nullptr) {
        auto rtl_get_version =
            reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        if (rtl_get_version != nullptr) {
            RTL_OSVERSIONINFOW info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (rtl_get_version(&info) == 0) {
                return "Windows " + std::to_string(info.dwMajorVersion) + "." +
                       std::to_string(info.dwMinorVersion) + " build " +
                       std::to_string(info.dwBuildNumber);
            }
        }
    }
    return "Windows (version unavailable)";
}

std::uint64_t query_ram_bytes() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return static_cast<std::uint64_t>(status.ullTotalPhys);
    }
    return 0;
}

#elif defined(__linux__)

// Reads a whole small text file into a string. Used for the handful of
// single-line /proc and /sys files queried below; none of them are large
// enough to need anything more careful.
std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string query_os() {
    // PRETTY_NAME is the field every major distribution's os-release sets for
    // exactly this purpose - a human-readable, complete-sentence name. Falls
    // back to `uname -sr` equivalent (uname(2) fields) if the file is
    // missing, which happens on some minimal container base images.
    const std::string contents = read_file("/etc/os-release");
    const std::string key      = "PRETTY_NAME=";
    const std::size_t pos      = contents.find(key);
    if (pos != std::string::npos) {
        std::size_t start = pos + key.size();
        std::size_t end   = contents.find('\n', start);
        std::string value = contents.substr(start, (end == std::string::npos)
                                                        ? std::string::npos
                                                        : end - start);
        // The value is usually double-quoted.
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        if (!value.empty()) {
            return value;
        }
    }

    utsname uts{};
    if (uname(&uts) == 0) {
        return std::string(uts.sysname) + " " + uts.release;
    }
    return "Linux (version unavailable)";
}

std::uint64_t query_ram_bytes() {
    struct sysinfo info{};
    if (sysinfo(&info) == 0) {
        // sysinfo() reports in units of mem_unit bytes, not always 1 - true on
        // some 32-bit kernels where totalram alone would overflow.
        return static_cast<std::uint64_t>(info.totalram) *
               static_cast<std::uint64_t>(info.mem_unit);
    }
    return 0;
}

// Shared by sample_current_mhz() and nominal_max_mhz() below: cpufreq's
// scaling_cur_freq / cpuinfo_max_freq sysfs files are both a single integer
// in kHz, one per online logical processor.
double read_cpufreq_khz(const std::string& filename, std::uint32_t logical) {
    const std::string path =
        "/sys/devices/system/cpu/cpu" + std::to_string(logical) + "/cpufreq/" + filename;
    std::ifstream file(path);
    if (!file) {
        return 0.0;
    }
    double khz = 0.0;
    file >> khz;
    return file ? khz : 0.0;
}

#endif

std::string utc_timestamp() {
    const auto now  = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);

    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif

    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buffer);
}

} // namespace

Environment capture_environment() {
    Environment environment;

    const CpuTopology& topology = cpu_topology();
    environment.cpu_brand       = topology.brand;
    environment.logical_processors = topology.logical_processor_count;
    // Left at 0 when detection failed. Reporting an unknown core count honestly
    // is the difference between a labelled scaling result and a misleading one.
    environment.physical_cores = topology.detected ? topology.physical_core_count : 0;
    environment.smt            = topology.has_smt();
    environment.ram_bytes      = query_ram_bytes();
    environment.os             = query_os();

    const BuildInfo& build          = build_info();
    environment.compiler            = std::string(build.compiler);
    environment.build_config        = std::string(build.configuration);
    environment.thunderbolt_version = std::string(build.version);
    environment.debug_assertions    = build.debug_assertions;
    environment.address_sanitizer   = build.address_sanitizer;

    environment.timestamp_utc = utc_timestamp();
    return environment;
}

double sample_current_mhz() {
#if defined(_WIN32)
    std::vector<ProcessorPowerInformation> info;
    if (!query_processor_power(info) || info.empty()) {
        return 0.0;
    }
    double total = 0.0;
    for (const auto& processor : info) {
        total += static_cast<double>(processor.CurrentMhz);
    }
    return total / static_cast<double>(info.size());
#elif defined(__linux__)
    // Averaged across every logical processor, the same quantity the Windows
    // path reports - this is what the throttle flag in BenchmarkRunner
    // compares against a run's own median, so it must mean the same thing on
    // both platforms.
    const std::uint32_t count = cpu_topology().logical_processor_count;
    if (count == 0) {
        return 0.0;
    }
    double total   = 0.0;
    unsigned found = 0;
    for (std::uint32_t logical = 0; logical < count; ++logical) {
        const double khz = read_cpufreq_khz("scaling_cur_freq", logical);
        if (khz > 0.0) {
            total += khz / 1000.0;
            ++found;
        }
    }
    // Some environments (containers, certain VMs) expose no cpufreq driver at
    // all - 0.0 here is the honest "unknown", same as the Windows path
    // returns when CallNtPowerInformation fails.
    return found > 0 ? total / static_cast<double>(found) : 0.0;
#else
    return 0.0;
#endif
}

double nominal_max_mhz() {
#if defined(_WIN32)
    std::vector<ProcessorPowerInformation> info;
    if (!query_processor_power(info) || info.empty()) {
        return 0.0;
    }
    return static_cast<double>(info.front().MaxMhz);
#elif defined(__linux__)
    // One logical processor's ceiling stands in for the machine's, matching
    // the Windows path (info.front().MaxMhz) - on a symmetric multicore part
    // every core shares the same maximum, and this project does not target
    // asymmetric (big.LITTLE-style) parts.
    const double khz = read_cpufreq_khz("cpuinfo_max_freq", 0);
    return khz > 0.0 ? khz / 1000.0 : 0.0;
#else
    return 0.0;
#endif
}

} // namespace thunderbolt::bench

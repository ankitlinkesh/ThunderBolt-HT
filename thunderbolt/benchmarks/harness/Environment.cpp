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

#else  // !_WIN32

std::string   query_os() { return "unknown"; }
std::uint64_t query_ram_bytes() { return 0; }

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
#else
    return 0.0;
#endif
}

} // namespace thunderbolt::bench

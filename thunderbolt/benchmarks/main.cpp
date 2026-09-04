// Thunderbolt HT benchmark driver.
//
// S64's command shapes. Every run writes a machine-readable document carrying the
// full S87 reproducibility block, because a number without its machine, build and
// protocol cannot be checked by anyone later - including its author.
#include "experiments/Experiments.hpp"

#include <thunderbolt/api/BuildInfo.hpp>
#include <thunderbolt/cpu/topology/CpuTopology.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void print_usage() {
    std::printf(
        "thunderbolt-bench - runtime benchmarks\n"
        "\n"
        "usage: thunderbolt-bench --experiment <name> [options]\n"
        "\n"
        "experiments:\n"
        "  granularity   Hold total work constant, vary the number of tasks it is split\n"
        "                into. The slope of time against task count is the per-task\n"
        "                scheduler cost. Answers: at what granularity does scheduling\n"
        "                overhead outweigh parallelism?\n"
        "  scaling       Sweep worker counts from 1 upward, reporting speedup and\n"
        "                efficiency, labelling where SMT siblings begin.\n"
        "\n"
        "options:\n"
        "  --workers N        Worker threads (default: one per logical processor)\n"
        "  --work-units N     Total work units held constant (default: 4000000)\n"
        "  --reps N           Measured repetitions per leg (default: 20)\n"
        "  --warmup N         Discarded warmup rounds (default: 3)\n"
        "  --cooldown-ms N    Pause between samples (default: 25)\n"
        "  --out PATH         Write the results document here\n"
        "\n"
        "The protocol is fixed and not configurable: legs are interleaved A/B/A/B,\n"
        "results are reported as median and IQR, and samples taken while the CPU was\n"
        "clocked below its nominal range are flagged rather than averaged in.\n");
}

bool parse_uint(const char* text, unsigned long long& out) {
    char*                    end   = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    using namespace thunderbolt;
    using namespace thunderbolt::bench;

    std::string       experiment;
    ExperimentOptions options;

    for (int i = 1; i < argc; ++i) {
        const char* arg  = argv[i];
        const bool  more = (i + 1) < argc;

        auto take_uint = [&](unsigned long long& target) {
            if (!more || !parse_uint(argv[i + 1], target)) {
                std::fprintf(stderr, "error: %s needs a number\n", arg);
                std::exit(2);
            }
            ++i;
        };

        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            print_usage();
            return 0;
        }
        if (std::strcmp(arg, "--experiment") == 0 && more) {
            experiment = argv[++i];
        } else if (std::strcmp(arg, "--workers") == 0) {
            unsigned long long value = 0;
            take_uint(value);
            options.workers = static_cast<std::uint32_t>(value);
        } else if (std::strcmp(arg, "--work-units") == 0) {
            unsigned long long value = 0;
            take_uint(value);
            options.work_units = value;
        } else if (std::strcmp(arg, "--reps") == 0) {
            unsigned long long value = 0;
            take_uint(value);
            options.run.repetitions = static_cast<int>(value);
        } else if (std::strcmp(arg, "--warmup") == 0) {
            unsigned long long value = 0;
            take_uint(value);
            options.run.warmup = static_cast<int>(value);
        } else if (std::strcmp(arg, "--cooldown-ms") == 0) {
            unsigned long long value = 0;
            take_uint(value);
            options.run.cooldown = std::chrono::milliseconds(static_cast<long long>(value));
        } else if (std::strcmp(arg, "--submission") == 0 && more) {
            const std::string mode = argv[++i];
            if (mode == "forkjoin") {
                options.submission = SubmissionMode::ForkJoin;
            } else if (mode == "external") {
                options.submission = SubmissionMode::External;
            } else {
                std::fprintf(stderr, "error: --submission must be forkjoin or external\n");
                return 2;
            }
        } else if (std::strcmp(arg, "--out") == 0 && more) {
            options.output_path = argv[++i];
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n\n", arg);
            print_usage();
            return 2;
        }
    }

    if (experiment.empty()) {
        print_usage();
        return 2;
    }

    const auto& build = build_info();

    // Refuse to present a debug or sanitizer build as a performance measurement.
    // The numbers would be real, but they would not be the numbers anyone means.
    if (build.debug_assertions || build.address_sanitizer) {
        std::fprintf(stderr,
                     "WARNING: this is a %s build with %s%s. Timings from it are NOT\n"
                     "         performance results and must not be published as such.\n\n",
                     std::string(build.configuration).c_str(),
                     build.debug_assertions ? "assertions enabled" : "",
                     build.address_sanitizer ? " AddressSanitizer" : "");
    }

    std::printf("Thunderbolt HT %.*s  [%.*s, %.*s]\n",
                static_cast<int>(build.version.size()), build.version.data(),
                static_cast<int>(build.compiler.size()), build.compiler.data(),
                static_cast<int>(build.configuration.size()), build.configuration.data());
    std::printf("%s\n", describe_topology(cpu_topology()).c_str());

    if (experiment == "granularity") {
        return run_granularity(options);
    }
    if (experiment == "scaling") {
        return run_scaling(options);
    }

    std::fprintf(stderr, "error: unknown experiment '%s'\n\n", experiment.c_str());
    print_usage();
    return 2;
}

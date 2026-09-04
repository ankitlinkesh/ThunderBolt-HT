#include "Experiments.hpp"

#include "../harness/ResultWriter.hpp"
#include "../workloads/Workloads.hpp"

#include <thunderbolt/cpu/topology/CpuTopology.hpp>
#include <thunderbolt/runtime/StandardRuntime.hpp>
#include <thunderbolt/runtime/ThunderboltRuntime.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

namespace thunderbolt::bench {
namespace {

constexpr std::uint32_t kTaskCapacity = 65536;

// Enough tasks that every worker count in the sweep has work to balance, few
// enough that per-task overhead is not the dominant term. The granularity
// experiment is where overhead is measured; this one is about scaling.
constexpr std::size_t kTaskCount = 512;

template <typename RuntimeT>
Leg make_leg(std::string name, std::uint32_t workers, ChunkedWorkload& workload) {
    Leg leg;
    leg.name     = std::move(name);
    leg.run_once = [workers, &workload] {
        RuntimeConfig config;
        config.worker_count  = workers;
        config.task_capacity = kTaskCapacity;
        RuntimeT runtime(config);

        return time_seconds([&] {
            const std::size_t chunks = workload.chunk_count();
            for (std::size_t i = 0; i < chunks; ++i) {
                (void)runtime.submit([&workload, i] { workload.run_chunk(i); });
            }
            runtime.wait_all();
        });
    };
    return leg;
}

} // namespace

int run_scaling(const ExperimentOptions& options) {
    const CpuTopology& topology = cpu_topology();
    const std::uint32_t max_workers =
        (options.workers != 0)
            ? options.workers
            : (topology.logical_processor_count > 0 ? topology.logical_processor_count : 1u);

    const Environment environment = capture_environment();

    std::printf("scaling: %llu work units in %zu tasks, 1..%u workers, %d reps\n",
                static_cast<unsigned long long>(options.work_units), kTaskCount, max_workers,
                options.run.repetitions);

    if (topology.detected && topology.physical_core_count > 0) {
        std::printf("physical cores: %u, logical processors: %u\n",
                    topology.physical_core_count, topology.logical_processor_count);
        std::printf("NOTE: worker counts above %u share physical cores via SMT. A drop in\n"
                    "      efficiency there is expected and is NOT a scheduler defect.\n\n",
                    topology.physical_core_count);
    } else {
        std::printf("physical core count unknown - efficiency cannot be attributed to SMT\n\n");
    }

    std::printf("%8s  %8s  %14s  %9s  %10s  %14s  %9s  %10s\n", "workers", "domain",
                "standard (s)", "std x", "std eff", "thunderbolt(s)", "tb x", "tb eff");

    struct Row {
        std::uint32_t workers;
        RunReport     report;
    };
    std::vector<Row> rows;

    double standard_serial    = 0.0;
    double thunderbolt_serial = 0.0;

    for (std::uint32_t workers = 1; workers <= max_workers; ++workers) {
        ChunkedWorkload workload(options.work_units, kTaskCount);

        std::vector<Leg> legs;
        legs.push_back(make_leg<StandardRuntime>("standard", workers, workload));
        legs.push_back(make_leg<ThunderboltRuntime>("thunderbolt", workers, workload));

        RunReport report = run_interleaved(legs, options.run);

        const double standard    = report.legs[0].timing.median;
        const double thunderbolt = report.legs[1].timing.median;

        if (workers == 1) {
            standard_serial    = standard;
            thunderbolt_serial = thunderbolt;
        }

        const double standard_speedup =
            (standard > 0.0) ? standard_serial / standard : 0.0;
        const double thunderbolt_speedup =
            (thunderbolt > 0.0) ? thunderbolt_serial / thunderbolt : 0.0;

        // Labelled so the SMT region is never mistaken for a scheduling failure.
        const bool in_smt_domain = topology.detected && topology.physical_core_count > 0 &&
                                   workers > topology.physical_core_count;
        const char* domain = in_smt_domain ? "SMT" : "cores";

        std::printf("%8u  %8s  %14.6f  %9.2f  %10.2f  %14.6f  %9.2f  %10.2f\n", workers, domain,
                    standard, standard_speedup, standard_speedup / workers, thunderbolt,
                    thunderbolt_speedup, thunderbolt_speedup / workers);

        Row row;
        row.workers = workers;
        row.report  = std::move(report);
        rows.push_back(std::move(row));
    }

    std::ostringstream document;
    JsonWriter         json(document);

    begin_result_document(json, "scaling", environment, rows.front().report);
    json.field("task_count", static_cast<std::uint64_t>(kTaskCount));
    json.field("work_units", options.work_units);

    json.begin_array("rows");
    for (const Row& row : rows) {
        json.begin_object();
        json.field("workers", static_cast<std::uint64_t>(row.workers));

        // Explicit, because S58's efficiency cliff past the physical core count is
        // a property of the hardware and must not be read as a scheduler result.
        const bool in_smt_domain = topology.detected && topology.physical_core_count > 0 &&
                                   row.workers > topology.physical_core_count;
        json.field("uses_smt_siblings", in_smt_domain);

        const double standard    = row.report.legs[0].timing.median;
        const double thunderbolt = row.report.legs[1].timing.median;
        json.field("speedup_standard", (standard > 0.0) ? standard_serial / standard : 0.0);
        json.field("speedup_thunderbolt",
                   (thunderbolt > 0.0) ? thunderbolt_serial / thunderbolt : 0.0);

        json.begin_array("legs");
        for (const LegResult& leg : row.report.legs) {
            write_leg(json, leg, /*include_samples=*/false);
        }
        json.end_array();
        json.end_object();
    }
    json.end_array();
    json.end_object();

    const std::string text = document.str();
    if (!options.output_path.empty()) {
        std::ofstream file(options.output_path);
        if (!file) {
            std::fprintf(stderr, "error: cannot write %s\n", options.output_path.c_str());
            return 1;
        }
        file << text << "\n";
        std::printf("\nwrote %s\n", options.output_path.c_str());
    }

    return 0;
}

} // namespace thunderbolt::bench

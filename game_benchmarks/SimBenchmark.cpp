#include "SimBenchmark.hpp"

#include "TaskflowSim.hpp"

#include <harness/BenchmarkRunner.hpp>
#include <harness/ResultWriter.hpp>

#include <engine/core/StateHash.hpp>

#include <thunderbolt/cpu/topology/CpuTopology.hpp>
#include <thunderbolt/runtime/StandardRuntime.hpp>
#include <thunderbolt/runtime/ThunderboltRuntime.hpp>

#if THUNDERBOLT_HAVE_TASKFLOW
#  include <taskflow/taskflow.hpp>
#endif

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>

namespace tbworld {
namespace {

using namespace thunderbolt;
using namespace thunderbolt::bench;

std::uint32_t resolve_workers(std::uint32_t requested) {
    if (requested != 0) {
        return requested;
    }
    const std::uint32_t logical = cpu_topology().logical_processor_count;
    return logical > 0 ? logical : 1u;
}

// Builds one leg. Everything expensive - the runtime, the executor, the world -
// is constructed INSIDE run_once but OUTSIDE the timed region, so a sample
// measures ticks and nothing else.
//
// A fresh runtime per sample is deliberate. Keeping all four legs' runtimes alive
// so they could be alternated cheaply would leave three sets of idle worker
// threads resident during the fourth's run, and a Thunderbolt worker spins before
// it parks. Construction costs about a millisecond and is excluded.
Leg make_leg(const std::string& name, const SimBenchmarkOptions& options,
             std::uint32_t workers) {
    Leg leg;
    leg.name = name;

    leg.run_once = [name, options, workers]() -> double {
        Simulation simulation(options.scene, options.seed);

        if (name == "serial") {
            return time_seconds([&] {
                for (std::uint64_t t = 0; t < options.ticks; ++t) {
                    simulation.tick_serial();
                }
            });
        }

#if THUNDERBOLT_HAVE_TASKFLOW
        if (name == "taskflow") {
            tf::Executor executor(static_cast<std::size_t>(workers));
            return time_seconds([&] {
                for (std::uint64_t t = 0; t < options.ticks; ++t) {
                    tick_with_taskflow(simulation, executor);
                }
            });
        }
#endif

        RuntimeConfig config;
        config.worker_count  = workers;
        config.task_capacity = 262144;
        config.affinity      = options.affinity;

        // S12 modes are selected by leg name, so a mode comparison is an ordinary
        // interleaved A/B rather than a separate code path with its own protocol.
        if (name == "thunderbolt_static") {
            config.scheduler = SchedulerMode::Static;
        } else if (name == "thunderbolt_aging") {
            config.scheduler = SchedulerMode::PriorityAging;
        } else if (name == "thunderbolt_pinned") {
            // S17 as a LEG, not a separate run. Comparing a pinned run against an
            // unpinned run measured minutes apart reintroduces exactly the
            // thermal drift interleaving exists to cancel - and on this part that
            // drift is larger than the effect. Measured both ways across separate
            // invocations first, got contradictory answers, and this is the fix.
            config.affinity = AffinityMode::TopologyAware;
        }

        std::unique_ptr<ITaskRuntime> runtime;
        if (name == "standard") {
            runtime = std::make_unique<StandardRuntime>(config);
        } else {
            runtime = std::make_unique<ThunderboltRuntime>(config);
        }

        return time_seconds([&] {
            for (std::uint64_t t = 0; t < options.ticks; ++t) {
                simulation.tick(*runtime);
            }
        });
    };

    return leg;
}

// Serial tick cost, for the workload-weight gate. A scene whose tick is not
// comfortably more expensive than the scheduling it triggers cannot say anything
// about scheduling, and its speedup must not be reported as one.
double measure_serial_tick(const SimBenchmarkOptions& options) {
    Simulation simulation(options.scene, options.seed);
    double     best = 0.0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const double seconds = time_seconds([&] {
            for (std::uint64_t t = 0; t < options.ticks; ++t) {
                simulation.tick_serial();
            }
        });
        if (best == 0.0 || seconds < best) {
            best = seconds;  // the least-disturbed observation
        }
    }
    return best / static_cast<double>(options.ticks);
}

} // namespace

int run_simulation_benchmark(const SimBenchmarkOptions& options) {
    const std::uint32_t workers = resolve_workers(options.workers);

    std::printf("scene=%s  vehicles=%zu npcs=%zu aircraft=%zu\n", options.scene.name,
                options.scene.vehicles, options.scene.npcs, options.scene.aircraft);
    std::printf("seed=%llu ticks=%llu workers=%u  reps=%d (+%d warmup, interleaved)\n\n",
                static_cast<unsigned long long>(options.seed),
                static_cast<unsigned long long>(options.ticks), workers, options.repetitions,
                options.warmup);

    const double serial_tick_ms = measure_serial_tick(options) * 1000.0;
    std::printf("serial tick T1 = %.4f ms\n", serial_tick_ms);

    // Advisory threshold. A tick this cheap is dominated by per-task scheduling
    // cost, so differences between runtimes say more about overhead than about
    // scheduling quality.
    const bool cpu_bound_enough = serial_tick_ms >= 0.5;
    if (!cpu_bound_enough) {
        std::printf("WARNING: this scene may be too cheap to be a scheduling benchmark.\n"
                    "         Per-tick scheduling is a large fraction of the work; prefer a\n"
                    "         heavier scene before quoting a speedup.\n");
    }
    std::printf("\n");

    std::vector<Leg> legs;
    legs.reserve(options.legs.size());
    for (const std::string& name : options.legs) {
        legs.push_back(make_leg(name, options, workers));
    }

    RunOptions run_options;
    run_options.repetitions = options.repetitions;
    run_options.warmup      = options.warmup;

    const RunReport report = run_interleaved(legs, run_options);

    std::printf("%-20s %12s %12s %11s %10s\n", "leg", "ms/tick", "IQR", "vs serial", "throttled");

    double serial_median = 0.0;
    for (const LegResult& leg : report.legs) {
        if (leg.name == "serial") {
            serial_median = leg.timing.median;
        }
    }

    for (const LegResult& leg : report.legs) {
        const double ms_per_tick = leg.timing.median * 1000.0 / static_cast<double>(options.ticks);
        const double iqr_ms      = leg.timing.iqr * 1000.0 / static_cast<double>(options.ticks);
        // "-" rather than 0.00x when no serial leg ran. A zero reads as a
        // measured zero speedup, a different and far more alarming claim.
        if (serial_median > 0.0 && leg.timing.median > 0.0) {
            std::printf("%-20s %12.4f %12.4f %10.2fx %10u\n", leg.name.c_str(),
                        ms_per_tick, iqr_ms, serial_median / leg.timing.median,
                        leg.throttled_count);
        } else {
            std::printf("%-20s %12.4f %12.4f %11s %10u\n", leg.name.c_str(),
                        ms_per_tick, iqr_ms, "-", leg.throttled_count);
        }
    }

    if (report.clock_unavailable) {
        std::printf("\nNOTE: clock frequency unreadable, so no claim is made about throttling\n"
                    "      in either direction.\n");
    }

    // Every leg must have produced identical world state. If they did not, the
    // timings are comparing different work and mean nothing.
    std::printf("\nverifying all legs agree on final state...\n");
    std::uint64_t reference_hash = 0;
    bool          first          = true;
    bool          all_agree      = true;
    for (const std::string& name : options.legs) {
        Simulation simulation(options.scene, options.seed);
        if (name == "serial") {
            for (std::uint64_t t = 0; t < options.ticks; ++t) {
                simulation.tick_serial();
            }
        }
#if THUNDERBOLT_HAVE_TASKFLOW
        else if (name == "taskflow") {
            tf::Executor executor(static_cast<std::size_t>(workers));
            for (std::uint64_t t = 0; t < options.ticks; ++t) {
                tick_with_taskflow(simulation, executor);
            }
        }
#endif
        else {
            RuntimeConfig config;
            config.worker_count  = workers;
            config.task_capacity = 262144;
            if (name == "thunderbolt_static") {
                config.scheduler = SchedulerMode::Static;
            } else if (name == "thunderbolt_aging") {
                config.scheduler = SchedulerMode::PriorityAging;
            }
            std::unique_ptr<ITaskRuntime> runtime;
            if (name == "standard") {
                runtime = std::make_unique<StandardRuntime>(config);
            } else {
                runtime = std::make_unique<ThunderboltRuntime>(config);
            }
            for (std::uint64_t t = 0; t < options.ticks; ++t) {
                simulation.tick(*runtime);
            }
        }

        const std::uint64_t hash = simulation.state_hash();
        if (first) {
            reference_hash = hash;
            first          = false;
        } else if (hash != reference_hash) {
            all_agree = false;
            std::printf("  MISMATCH: %s -> %s (expected %s)\n", name.c_str(),
                        hash_to_string(hash).c_str(), hash_to_string(reference_hash).c_str());
        }
    }
    std::printf(all_agree ? "  all legs agree: %s\n" : "  LEGS DISAGREE - timings are invalid\n",
                hash_to_string(reference_hash).c_str());

    if (!options.output_path.empty()) {
        std::ostringstream document;
        JsonWriter         json(document);
        const Environment  environment = capture_environment();

        begin_result_document(json, "simulation", environment, report);
        json.field("scene", options.scene.name);
        json.field("vehicles", static_cast<std::uint64_t>(options.scene.vehicles));
        json.field("npcs", static_cast<std::uint64_t>(options.scene.npcs));
        json.field("aircraft", static_cast<std::uint64_t>(options.scene.aircraft));
        json.field("seed", options.seed);
        json.field("ticks", options.ticks);
        json.field("workers", static_cast<std::uint64_t>(workers));

        json.begin_object("workload_gate");
        json.field("serial_tick_ms", serial_tick_ms);
        json.field("cpu_bound_enough", cpu_bound_enough);
        json.end_object();

        json.field("state_hash", hash_to_string(reference_hash));
        json.field("all_legs_agree", all_agree);

        json.begin_array("legs");
        for (const LegResult& leg : report.legs) {
            write_leg(json, leg, /*include_samples=*/true);
        }
        json.end_array();
        json.end_object();

        std::ofstream file(options.output_path);
        if (!file) {
            std::fprintf(stderr, "error: cannot write %s\n", options.output_path.c_str());
            return 1;
        }
        file << document.str() << "\n";
        std::printf("\nwrote %s\n", options.output_path.c_str());
    }

    return all_agree ? 0 : 1;
}

} // namespace tbworld

namespace tbworld {

namespace {

// One tick's duration, in milliseconds.
std::vector<double> capture_tick_times(const std::string& name,
                                       const SimBenchmarkOptions& options,
                                       std::uint32_t workers) {
    using namespace thunderbolt;
    using namespace thunderbolt::bench;

    Simulation          simulation(options.scene, options.seed);
    std::vector<double> ticks;
    ticks.reserve(static_cast<std::size_t>(options.ticks));

    RuntimeConfig config;
    config.worker_count  = workers;
    config.task_capacity = 262144;
    config.affinity      = options.affinity;
    if (name == "thunderbolt_static") {
        config.scheduler = SchedulerMode::Static;
    } else if (name == "thunderbolt_aging") {
        config.scheduler = SchedulerMode::PriorityAging;
    }

    std::unique_ptr<ITaskRuntime> runtime;
#if THUNDERBOLT_HAVE_TASKFLOW
    std::unique_ptr<tf::Executor> executor;
    if (name == "taskflow") {
        executor = std::make_unique<tf::Executor>(static_cast<std::size_t>(workers));
    }
#endif
    if (name == "standard") {
        runtime = std::make_unique<StandardRuntime>(config);
    } else if (name != "serial" && name != "taskflow") {
        runtime = std::make_unique<ThunderboltRuntime>(config);
    }

    for (std::uint64_t t = 0; t < options.ticks; ++t) {
        // Per-tick clock reads cost tens of nanoseconds against a tick of
        // hundreds of microseconds - about 0.01%, far below the effect being
        // measured.
        const double seconds = time_seconds([&] {
            if (name == "serial") {
                simulation.tick_serial();
            }
#if THUNDERBOLT_HAVE_TASKFLOW
            else if (name == "taskflow") {
                tick_with_taskflow(simulation, *executor);
            }
#endif
            else {
                simulation.tick(*runtime);
            }
        });
        ticks.push_back(seconds * 1000.0);
    }
    return ticks;
}

} // namespace

int run_frame_time_study(const SimBenchmarkOptions& options) {
    using namespace thunderbolt;
    using namespace thunderbolt::bench;

    const std::uint32_t workers = resolve_workers(options.workers);

    std::printf("frame-time distribution: scene=%s ticks=%llu workers=%u affinity=%s\n\n",
                options.scene.name, static_cast<unsigned long long>(options.ticks), workers,
                options.affinity == AffinityMode::Disabled ? "off" : "topology-aware");

    std::printf("%-20s %9s %9s %9s %9s %9s %11s\n", "leg", "median", "p95", "p99", "p99.9",
                "worst", "1pct low fps");

    std::ostringstream document;
    JsonWriter         json(document);
    json.begin_object();
    json.field("schema", "thunderbolt-benchmark-1");
    json.field("benchmark", "frame_time_distribution");
    write_environment(json, capture_environment());
    json.field("scene", options.scene.name);
    json.field("ticks", options.ticks);
    json.field("workers", static_cast<std::uint64_t>(workers));
    json.field("affinity", options.affinity == AffinityMode::Disabled ? "disabled"
                                                                     : "topology_aware");
    json.field("note",
               "Percentiles are over INDIVIDUAL TICKS, not run medians. The '1% low fps' column "
               "is the reciprocal of the 99th-percentile tick time - the frame rate the worst 1% "
               "of ticks sustain, not an average of them.");
    json.begin_array("legs");

    for (const std::string& name : options.legs) {
        std::vector<double> ticks = capture_tick_times(name, options, workers);
        if (ticks.empty()) {
            continue;
        }
        std::vector<double> sorted = ticks;
        std::sort(sorted.begin(), sorted.end());

        const double median = percentile_sorted(sorted, 0.50);
        const double p95    = percentile_sorted(sorted, 0.95);
        const double p99    = percentile_sorted(sorted, 0.99);
        const double p999   = percentile_sorted(sorted, 0.999);
        const double worst  = sorted.back();
        const double low1   = (p99 > 0.0) ? 1000.0 / p99 : 0.0;

        std::printf("%-20s %9.4f %9.4f %9.4f %9.4f %9.4f %11.0f\n", name.c_str(), median, p95, p99,
                    p999, worst, low1);

        json.begin_object();
        json.field("leg", name);
        json.field("tick_count", static_cast<std::uint64_t>(ticks.size()));
        json.field("median_ms", median);
        json.field("p95_ms", p95);
        json.field("p99_ms", p99);
        json.field("p999_ms", p999);
        json.field("worst_ms", worst);
        json.field("one_percent_low_fps", low1);
        // Tail-to-median ratio: how much worse the bad frames are than the
        // typical one. A scheduler with a better median and a worse ratio is
        // worse to play, which an average would hide entirely.
        json.field("p99_over_median", (median > 0.0) ? p99 / median : 0.0);
        json.end_object();
    }

    json.end_array();
    json.end_object();

    if (!options.output_path.empty()) {
        std::ofstream file(options.output_path);
        if (!file) {
            std::fprintf(stderr, "error: cannot write %s\n", options.output_path.c_str());
            return 1;
        }
        file << document.str() << "\n";
        std::printf("\nwrote %s\n", options.output_path.c_str());
    }
    return 0;
}

} // namespace tbworld

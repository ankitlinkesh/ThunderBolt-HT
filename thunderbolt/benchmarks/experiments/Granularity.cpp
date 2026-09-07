#include "Experiments.hpp"

#include "../harness/ResultWriter.hpp"
#include "../workloads/Workloads.hpp"

#include <thunderbolt/core/task/Task.hpp>
#include <thunderbolt/cpu/topology/CpuTopology.hpp>
#include <thunderbolt/runtime/StandardRuntime.hpp>
#include <thunderbolt/runtime/ThunderboltRuntime.hpp>

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#if THUNDERBOLT_HAVE_TASKFLOW
#  include <taskflow/taskflow.hpp>
#  include <taskflow/algorithm/for_each.hpp>
#endif

namespace thunderbolt::bench {
namespace {

// Task counts to sweep. Log-spaced, spanning "fewer tasks than workers" (where
// the limit is load imbalance) through "far more tasks than work" (where the
// limit is scheduler overhead). The interesting answer lives at the crossover.
const std::vector<std::size_t> kTaskCounts = {1,   2,   4,    8,    16,    64,
                                              256, 1024, 4096, 16384, 65536};

// Comfortably above the largest task count, so pool exhaustion never silently
// converts the experiment into a measurement of inline execution. The inline
// counter is reported anyway, so the assumption is checked rather than trusted.
constexpr std::uint32_t kTaskCapacity = 262144;

double run_one_sample(ITaskRuntime& runtime, ChunkedWorkload& workload, SubmissionMode mode) {
    // Timed region contains submission and completion only. Runtime construction,
    // allocation and result storage are all outside it.
    if (mode == SubmissionMode::External) {
        return time_seconds([&] {
            const std::size_t chunks = workload.chunk_count();
            for (std::size_t i = 0; i < chunks; ++i) {
                (void)runtime.submit([&workload, i] { workload.run_chunk(i); });
            }
            runtime.wait_all();
        });
    }

    // Fork-join: one root task spawns the rest. Its children go onto the root's
    // own deque - no lock - and reach the other workers by being stolen. This is
    // the path a frame graph takes, and the one where per-task cost is dispatch
    // rather than contention on a single shared queue.
    //
    // The root waits on CHILD HANDLES, not wait_all(). wait_all() from inside a
    // task can never return: the calling task is itself outstanding, so the count
    // has a permanent floor of one. That mistake was made here first and now
    // asserts in the runtime.
    //
    // The handle buffer is reserved OUTSIDE the timed region, so the measurement
    // does not include a reallocation the runtime is not responsible for.
    std::vector<TaskHandle> handles;
    handles.reserve(workload.chunk_count());

    return time_seconds([&] {
        TaskHandle root = runtime.submit([&runtime, &workload, &handles](TaskContext&) {
            const std::size_t chunks = workload.chunk_count();
            handles.clear();
            for (std::size_t i = 0; i < chunks; ++i) {
                handles.push_back(runtime.submit([&workload, i] { workload.run_chunk(i); }));
            }
            for (TaskHandle handle : handles) {
                runtime.wait(handle);  // helps rather than parking: we are on a worker
            }
        });
        runtime.wait(root);
    });
}

// Phase I Stage 6: parallel_for's OWN dynamic-partitioning path, run through
// runtime.parallel_for(0, chunks, grain, ...) rather than one submit() per
// chunk. This is the leg comparable to taskflow_for_each below - both hand the
// scheduler a range and let IT decide how many TASKS to run; grain then decides
// how many CLAIMS those tasks make, which is a caller knob on both sides (this
// project's grain, Taskflow's partitioner) rather than something either
// scheduler picks for you.
//
// grain=1 was tried first, to match taskflow_for_each's literal step=1 - and it
// was the wrong comparison. Taskflow's default partitioner does not actually
// claim one index at a time; it chunks internally regardless of the step
// argument, which only controls the index stride. Handing OUR partitioner
// grain=1 while Taskflow silently chunks made every claim on our side pay a
// full contended fetch_add for one unit of work, which is the atomic-cursor
// equivalent of the granularity trap S85 exists to measure - so the fairer
// comparison uses a grain sized the same way a chunking partitioner would: a
// bounded number of claims per worker, not one claim per element.
[[nodiscard]] std::size_t partitioned_grain(std::size_t chunks, std::uint32_t workers) {
    constexpr std::size_t kClaimsPerWorker = 32;
    const std::size_t     divisor = static_cast<std::size_t>(workers) * kClaimsPerWorker;
    const std::size_t     grain   = (chunks + divisor - 1) / divisor;
    return grain == 0 ? 1 : grain;
}

template <typename RuntimeT>
double run_partitioned_sample(RuntimeT& runtime, ChunkedWorkload& workload, std::size_t grain) {
    return time_seconds([&] {
        const std::size_t chunks = workload.chunk_count();
        runtime.parallel_for(std::size_t{0}, chunks, grain,
                             [&workload](std::size_t lo, std::size_t hi) {
                                 for (std::size_t i = lo; i < hi; ++i) {
                                     workload.run_chunk(i);
                                 }
                             });
    });
}

template <typename RuntimeT>
Leg make_partitioned_leg(std::string name, std::uint32_t workers, ChunkedWorkload& workload,
                         std::uint64_t& inline_executions) {
    Leg leg;
    leg.name = std::move(name);
    leg.run_once = [workers, &workload, &inline_executions] {
        RuntimeConfig config;
        config.worker_count   = workers;
        config.task_capacity  = kTaskCapacity;
        config.deque_capacity = 131072;

        RuntimeT runtime(config);
        const std::size_t grain = partitioned_grain(workload.chunk_count(), workers);
        const double seconds = run_partitioned_sample(runtime, workload, grain);
        inline_executions += runtime.inline_execution_count();
        return seconds;
    };
    return leg;
}

// Pool statistics from the most recent sample. The free-list mutex is taken
// twice per task, so it is the leading suspect for high per-task cost - and a
// suspect gets checked against a counter, not reasoned about.
struct PoolProbe {
    std::uint64_t acquires  = 0;
    std::uint64_t contended = 0;
};

template <typename RuntimeT>
Leg make_leg(std::string name, std::uint32_t workers, ChunkedWorkload& workload,
             std::uint64_t& inline_executions, SubmissionMode mode, PoolProbe& probe,
             std::uint32_t optimizations = kOptNone) {
    Leg leg;
    leg.name = std::move(name);
    leg.run_once = [workers, &workload, &inline_executions, mode, &probe, optimizations] {
        RuntimeConfig config;
        config.worker_count  = workers;
        config.task_capacity = kTaskCapacity;
        // Large enough that a fork-join burst does not overflow into the global
        // queue, which would quietly turn this back into the External case.
        config.deque_capacity = 131072;
        config.optimizations  = optimizations;

        // A fresh runtime per sample. Reusing one across samples would let a
        // previous sample's parked threads and warmed deques leak into the next,
        // and keeping two runtimes alive so they could be alternated cheaply
        // would leave the idle one's workers resident.
        RuntimeT runtime(config);

        const double seconds = run_one_sample(runtime, workload, mode);
        inline_executions += runtime.inline_execution_count();
        probe.acquires  = runtime.pool_acquire_count();
        probe.contended = runtime.pool_contended_lock_count();
        return seconds;
    };
    // Pool statistics from the last measured sample. The free-list mutex is
    // taken twice per task, so if per-task cost is high this is the first
    // hypothesis - and it gets checked against a counter rather than argued.
    leg.record_counters = [&probe](ResultRecorder& recorder) {
        recorder.count("pool_acquires", probe.acquires);
        recorder.count("pool_contended_locks", probe.contended);
    };

    return leg;
}

#if THUNDERBOLT_HAVE_TASKFLOW

// The external reference, run TWO ways - and the distinction matters for
// fairness. Handing Taskflow N explicit tasks compares like with like, but it is
// not how Taskflow is meant to be used: its parallel algorithms partition the
// range themselves. Reporting only the first would make it look bad for reasons
// that have nothing to do with scheduling quality, which is exactly the
// straw-man problem this leg was added to avoid. So both are measured and
// labelled separately.

Leg make_taskflow_explicit_leg(std::uint32_t workers, ChunkedWorkload& workload) {
    Leg leg;
    leg.name     = "taskflow_explicit";
    leg.run_once = [workers, &workload] {
        // The EXECUTOR is built outside the timed region - it is a thread pool,
        // the counterpart of constructing a Thunderbolt runtime.
        tf::Executor executor(workers);

        // The GRAPH is built INSIDE it, and that correction matters more than
        // anything else in this file. It was previously outside, on the stated
        // reasoning that this "matched how the Thunderbolt legs exclude runtime
        // construction" - a false equivalence. Thunderbolt's submit() allocates a
        // pool slot and moves the task body, and that is inside its timed region
        // because per-task cost is exactly what this experiment measures. Timing
        // Taskflow's execution but not its task creation compared task
        // creation + scheduling against scheduling alone, and inflated the
        // reported gap.
        return time_seconds([&] {
            tf::Taskflow      flow;
            const std::size_t chunks = workload.chunk_count();
            for (std::size_t i = 0; i < chunks; ++i) {
                flow.emplace([&workload, i] { workload.run_chunk(i); });
            }
            executor.run(flow).wait();
        });
    };
    return leg;
}

Leg make_taskflow_native_leg(std::uint32_t workers, ChunkedWorkload& workload) {
    Leg leg;
    leg.name     = "taskflow_for_each";
    leg.run_once = [workers, &workload] {
        tf::Executor executor(workers);
        // Graph construction timed, for the same reason as the explicit leg.
        return time_seconds([&] {
            tf::Taskflow      flow;
            const std::size_t chunks = workload.chunk_count();
            // Taskflow chooses its own partitioning here - its best case.
            flow.for_each_index(std::size_t{0}, chunks, std::size_t{1},
                                [&workload](std::size_t i) { workload.run_chunk(i); });
            executor.run(flow).wait();
        });
    };
    return leg;
}

#endif  // THUNDERBOLT_HAVE_TASKFLOW

// Serial reference for the workload-weight gate. If T1 is not comfortably larger
// than the scheduler's own per-frame cost, the scene is too cheap to say anything
// about scheduling and its speedup must not be reported.
double measure_serial_baseline(std::uint64_t work_units) {
    ChunkedWorkload workload(work_units, 1);
    double best = 0.0;
    for (int i = 0; i < 5; ++i) {
        const double seconds = time_seconds([&] { workload.run_chunk(0); });
        if (best == 0.0 || seconds < best) {
            best = seconds;  // minimum: the least-disturbed observation
        }
    }
    return best;
}

} // namespace

int run_granularity(const ExperimentOptions& options) {
    const CpuTopology& topology = cpu_topology();
    const std::uint32_t workers =
        (options.workers != 0)
            ? options.workers
            : (topology.logical_processor_count > 0 ? topology.logical_processor_count : 1u);

    const Environment environment = capture_environment();
    const char* mode_name =
        (options.submission == SubmissionMode::ForkJoin) ? "fork-join" : "external";

    std::printf("granularity: %llu work units, %u workers, %d reps\n",
                static_cast<unsigned long long>(options.work_units), workers,
                options.run.repetitions);

    const double serial_seconds = measure_serial_baseline(options.work_units);
    std::printf("serial baseline T1 = %.6f s\n\n", serial_seconds);

#if THUNDERBOLT_HAVE_TASKFLOW
    std::printf("%10s  %13s  %13s  %13s  %13s\n", "tasks", "standard(s)",
                "thunderbolt", "tf_explicit", "tf_foreach");
#else
    std::printf("%10s  %13s  %13s   (no external reference leg; configure with -DTHUNDERBOLT_REFERENCE_RUNTIMES=ON)\n",
                "tasks", "standard(s)", "thunderbolt");
#endif

    std::ostringstream document;
    JsonWriter         json(document);

    RunReport last_report;  // for the shared header
    bool      header_written = false;

    std::vector<double> fit_x_standard, fit_y_standard;
    std::vector<double> fit_x_thunderbolt, fit_y_thunderbolt;
    // leg name -> (task counts, medians), so every leg including the ablation
    // ones gets a per-task slope rather than only the two originals.
    std::vector<std::string>         fit_names;
    std::vector<std::vector<double>> fit_xs, fit_ys;

    struct Row {
        std::size_t task_count;
        RunReport   report;
        std::uint64_t inline_standard    = 0;
        std::uint64_t inline_thunderbolt = 0;
    };
    std::vector<Row> rows;

    for (std::size_t task_count : kTaskCounts) {
        ChunkedWorkload workload(options.work_units, task_count);

        std::uint64_t inline_standard    = 0;
        std::uint64_t inline_thunderbolt = 0;
        std::uint64_t inline_partitioned = 0;
        PoolProbe     probe_standard;
        PoolProbe     probe_thunderbolt;

        std::vector<Leg> legs;
        legs.push_back(make_leg<StandardRuntime>("standard", workers, workload, inline_standard,
                                                 options.submission, probe_standard));
        legs.push_back(make_leg<ThunderboltRuntime>("thunderbolt", workers, workload,
                                                    inline_thunderbolt, options.submission,
                                                    probe_thunderbolt));
        // Stage 6: the dynamic-partitioning path, the one comparable to
        // taskflow_for_each below rather than to taskflow_explicit above -
        // both hand the scheduler a range instead of a fixed task count.
        legs.push_back(make_partitioned_leg<ThunderboltRuntime>("tb_partitioned", workers,
                                                                workload, inline_partitioned));
        // Ablation legs. Interleaved against the baseline in the same run, which
        // is the only way a few-percent difference is distinguishable from
        // thermal drift on this machine.
        legs.push_back(make_leg<ThunderboltRuntime>("tb_o1_skipempty", workers, workload,
                                                    inline_thunderbolt, options.submission,
                                                    probe_thunderbolt, kOptSkipEmptyDeques));
        legs.push_back(make_leg<ThunderboltRuntime>("tb_o2_onebarrier", workers, workload,
                                                    inline_thunderbolt, options.submission,
                                                    probe_thunderbolt,
                                                    kOptSingleBarrierOnComplete));
        legs.push_back(make_leg<ThunderboltRuntime>(
            "tb_o12_both", workers, workload, inline_thunderbolt, options.submission,
            probe_thunderbolt, kOptSkipEmptyDeques | kOptSingleBarrierOnComplete));
#if THUNDERBOLT_HAVE_TASKFLOW
        legs.push_back(make_taskflow_explicit_leg(workers, workload));
        legs.push_back(make_taskflow_native_leg(workers, workload));
#endif

        RunReport report = run_interleaved(legs, options.run);

        const double standard_median    = report.legs[0].timing.median;
        const double thunderbolt_median = report.legs[1].timing.median;

        // Every leg gets a column, so an added reference leg cannot silently
        // run without appearing in the table.
        std::printf("%10zu  %13.6f  %13.6f", task_count, standard_median,
                    thunderbolt_median);
        for (std::size_t leg_index = 2; leg_index < report.legs.size(); ++leg_index) {
            std::printf("  %13.6f", report.legs[leg_index].timing.median);
        }
        std::printf("\n");

        // Fit only where there are enough tasks to keep every worker busy. Below
        // that, total time is dominated by load imbalance rather than by
        // per-task cost, and including those points would bias the slope.
        if (task_count >= static_cast<std::size_t>(workers) * 8) {
            fit_x_standard.push_back(static_cast<double>(task_count));
            fit_y_standard.push_back(standard_median);
            fit_x_thunderbolt.push_back(static_cast<double>(task_count));
            fit_y_thunderbolt.push_back(thunderbolt_median);

            if (fit_names.empty()) {
                for (const LegResult& leg : report.legs) {
                    fit_names.push_back(leg.name);
                    fit_xs.emplace_back();
                    fit_ys.emplace_back();
                }
            }
            for (std::size_t li = 0; li < report.legs.size() && li < fit_names.size(); ++li) {
                fit_xs[li].push_back(static_cast<double>(task_count));
                fit_ys[li].push_back(report.legs[li].timing.median);
            }
        }

        Row row;
        row.task_count         = task_count;
        row.report             = std::move(report);
        row.inline_standard    = inline_standard;
        row.inline_thunderbolt = inline_thunderbolt;
        rows.push_back(std::move(row));

        if (!header_written) {
            last_report    = rows.back().report;
            header_written = true;
        }
    }

    const LinearFit fit_standard    = fit_linear(fit_x_standard, fit_y_standard);
    const LinearFit fit_thunderbolt = fit_linear(fit_x_thunderbolt, fit_y_thunderbolt);

    std::printf("\nper-task scheduler cost (slope of total time vs task count):\n");
    if (fit_standard.valid) {
        std::printf("  standard    : %8.1f ns/task   (R^2 = %.3f)\n",
                    fit_standard.slope * 1e9, fit_standard.r_squared);
    }
    if (fit_thunderbolt.valid) {
        std::printf("  thunderbolt : %8.1f ns/task   (R^2 = %.3f)\n",
                    fit_thunderbolt.slope * 1e9, fit_thunderbolt.r_squared);
    }
    std::printf("\nper-leg per-task cost:\n");
    for (std::size_t li = 0; li < fit_names.size(); ++li) {
        const LinearFit leg_fit = fit_linear(fit_xs[li], fit_ys[li]);
        if (leg_fit.valid) {
            std::printf("  %-20s %8.0f ns/task   (R^2 = %.3f)\n",
                        fit_names[li].c_str(), leg_fit.slope * 1e9, leg_fit.r_squared);
        }
    }

    std::printf("\nA low R^2 means the linear-overhead model does not describe the data,\n"
                "and the slope must not be quoted as a per-task cost.\n");

    // ---- results document ------------------------------------------------
    begin_result_document(json, "granularity", environment, last_report);

    json.field("workers", static_cast<std::uint64_t>(workers));
    json.field("work_units", options.work_units);
    // Recorded because it determines WHICH cost is being measured - dispatch, or
    // contention on the shared injection queue.
    json.field("submission_mode", mode_name);

    // The task slab's memory footprint, recorded because it turned out to matter
    // more than lock contention. At high task counts every slot touch is a cache
    // miss once the live set exceeds last-level cache.
    json.field("task_bytes", static_cast<std::uint64_t>(sizeof(Task)));
    json.field("task_slab_bytes",
               static_cast<std::uint64_t>(sizeof(Task)) * kTaskCapacity);
#if THUNDERBOLT_HAVE_TASKFLOW
    json.field("reference_runtime", TASKFLOW_VERSION_STRING);
#else
    // Stated explicitly rather than left out: without an external reference, a
    // speedup over this project's own baseline is open to the straw-man
    // objection, and a results file must say so on its face.
    json.field("reference_runtime", "none");
#endif

    json.begin_object("serial_baseline");
    json.field("t1_seconds", serial_seconds);
    // The gate: a workload this cheap cannot say anything about scheduling.
    json.field("cpu_bound_gate_passed", serial_seconds > 0.001);
    json.field("gate_note",
               "T1 must be comfortably above per-frame scheduler cost, or the configuration "
               "measures noise rather than scheduling.");
    json.end_object();

    json.begin_array("rows");
    for (const Row& row : rows) {
        json.begin_object();
        json.field("task_count", static_cast<std::uint64_t>(row.task_count));
        json.field("units_per_task",
                   static_cast<std::uint64_t>(options.work_units / row.task_count));

        json.begin_array("legs");
        for (const LegResult& leg : row.report.legs) {
            write_leg(json, leg, /*include_samples=*/false);
        }
        json.end_array();

        // Non-zero here means the pool ran dry and some tasks executed inline,
        // reducing parallelism. Reported so the row can be discounted rather
        // than silently believed.
        json.field("inline_executions_standard", row.inline_standard);
        json.field("inline_executions_thunderbolt", row.inline_thunderbolt);
        json.end_object();
    }
    json.end_array();

    json.begin_object("analysis");
    json.begin_object("per_task_overhead_ns");
    if (fit_standard.valid) {
        json.field("standard", fit_standard.slope * 1e9);
        json.field("standard_r_squared", fit_standard.r_squared);
    }
    if (fit_thunderbolt.valid) {
        json.field("thunderbolt", fit_thunderbolt.slope * 1e9);
        json.field("thunderbolt_r_squared", fit_thunderbolt.r_squared);
    }
    json.end_object();
    json.field("method",
               "Total work is held constant while the task count varies, so the slope of total "
               "time against task count is the marginal cost of one task. Fitted only over task "
               "counts of at least 8x the worker count, below which load imbalance dominates.");
    json.end_object();

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

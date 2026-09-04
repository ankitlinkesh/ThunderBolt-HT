// Thunderbolt HT - the experiments themselves.
#pragma once

#include "../harness/BenchmarkRunner.hpp"

#include <cstdint>
#include <string>

namespace thunderbolt::bench {

// How the tasks are handed to the runtime. This is not a detail: it changes
// which part of the runtime is being measured, and reporting one while meaning
// the other is how a benchmark ends up describing lock contention as scheduler
// overhead.
enum class SubmissionMode {
    // The submitting thread is OUTSIDE the runtime, so it owns no deque and every
    // task goes through the shared injection queue. This measures external
    // submission throughput under contention - a real pattern (a frame loop
    // handing over a batch), but lock-bound by construction at high task counts.
    External,

    // A root task spawns the children, so they land on ITS worker's deque with no
    // lock at all and migrate only by stealing. This is the fork-join pattern a
    // frame graph actually uses, and the one that isolates dispatch cost.
    ForkJoin,
};

struct ExperimentOptions {
    SubmissionMode submission = SubmissionMode::ForkJoin;

    // 0 means "one worker per logical processor".
    std::uint32_t workers = 0;

    // Total work units, held constant across every configuration in an
    // experiment. Only the DECOMPOSITION varies - that is what makes the
    // granularity result a measurement of scheduling rather than of workload.
    std::uint64_t work_units = 4'000'000;

    RunOptions  run;
    std::string output_path;  // empty means stdout only
};

// S85 / S59.6 / S59.7. Holds total work constant and varies the number of tasks
// it is split into, from one task to tens of thousands. The slope of total time
// against task count IS the per-task scheduler cost, recovered without
// instrumenting anything - which matters because timestamping a 200 ns task with
// a 30 ns clock read would perturb the quantity being measured.
int run_granularity(const ExperimentOptions& options);

// S58. Sweeps worker counts from 1 upward, reporting speedup and efficiency, and
// labelling where the physical cores end and SMT siblings begin.
int run_scaling(const ExperimentOptions& options);

} // namespace thunderbolt::bench

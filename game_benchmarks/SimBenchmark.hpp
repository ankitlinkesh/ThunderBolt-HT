// Simulation A/B measurement, through the real harness.
//
// WHY THIS EXISTS. `thunderbolt-sim` originally timed one run with one stopwatch
// call. Repeated sampling showed 13-62% spread between identical runs on this
// 15 W part, which means single-shot simulation numbers were noise dressed as
// results - and a "2.2x faster" figure was published from them before that was
// noticed.
//
// Everything needed to do it properly already existed in
// thunderbolt/benchmarks/harness: interleaved A/B/A/B ordering, discarded warmup,
// median and IQR instead of a mean, clock sampling so throttled samples are
// flagged rather than averaged in, and the full reproducibility block. The bug
// was building a second measurement path that did not use any of it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <engine/core/Simulation.hpp>

#include <thunderbolt/api/RuntimeConfig.hpp>

namespace tbworld {

struct SimBenchmarkOptions {
    SceneSpec     scene{};
    std::uint64_t seed    = 42;
    std::uint64_t ticks   = 200;
    std::uint32_t workers = 0;

    // Legs to compare, by runtime name. Order does not affect the result: the
    // harness interleaves them.
    std::vector<std::string> legs;

    int         repetitions = 8;
    int         warmup      = 2;
    std::string output_path;

    // S17. Held identical across every leg, so pinning is a variable under test
    // rather than one runtime's private advantage.
    thunderbolt::AffinityMode affinity = thunderbolt::AffinityMode::Disabled;
};

// Runs every leg interleaved and reports medians. Returns a process exit code.
int run_simulation_benchmark(const SimBenchmarkOptions& options);

// S59.8 / S57. Times every tick individually and reports the DISTRIBUTION -
// median, 99th and 99.9th percentile, worst - rather than an average.
//
// A separate study because it needs different data. The A/B path reports IQR
// across run MEDIANS, which says how reproducible a run is and nothing about how
// bad the worst frames were. For a real-time system the tail is the number that
// matters: a scheduler with a better mean and a worse 1% low is worse to play.
int run_frame_time_study(const SimBenchmarkOptions& options);

} // namespace tbworld

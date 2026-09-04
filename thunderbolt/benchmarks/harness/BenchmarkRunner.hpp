// Thunderbolt HT - the measurement protocol.
//
// This file is where the project's performance claims either become trustworthy
// or do not. Three things it enforces, each for a specific reason:
//
// 1. INTERLEAVING. Legs run A/B/A/B, never all-of-A-then-all-of-B. On a 15 W part
//    the sustained clock falls as a run proceeds, so a batched schedule
//    systematically penalises whichever leg went second - by more than the effect
//    being measured. Interleaving spreads that drift evenly across legs.
//
// 2. A FRESH RUNTIME PER SAMPLE. Keeping two runtimes alive so they can be
//    alternated cheaply would leave the idle one's worker threads resident, and a
//    Thunderbolt worker spins before parking. Constructing per sample costs about
//    a millisecond of thread creation, which is excluded from the timed region,
//    and removes the confound entirely.
//
// 3. VISIBLE THROTTLING. Clock frequency is sampled around every run. A sample
//    taken while the CPU was well below its nominal clock is FLAGGED rather than
//    quietly averaged in, because the alternative is reporting a scheduler result
//    that is really a thermal one.
#pragma once

#include "Environment.hpp"
#include "Statistics.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace thunderbolt::bench {

// One timed observation.
struct Sample {
    double seconds        = 0.0;
    double clock_mhz      = 0.0;  // averaged across the run; 0 when unavailable
    bool   throttled      = false;
    bool   warmup         = false;
};

// One thing being compared - a runtime, a scheduler mode, a worker count.
struct Leg {
    std::string name;

    // Runs the workload once and returns the elapsed seconds of the WORKLOAD
    // ONLY. Setup and teardown (including runtime construction) belong inside
    // this call but outside its timed region.
    std::function<double()> run_once;

    // Optional: extra counters to record alongside the timing, gathered after the
    // final measured sample. Used for steal counts, overflow counts and the like.
    std::function<void(class ResultRecorder&)> record_counters;
};

struct LegResult {
    std::string         name;
    std::vector<Sample> samples;   // measured samples only; warmups excluded
    Summary             timing;    // over measured, non-throttled samples
    Summary             timing_including_throttled;
    std::uint32_t       throttled_count = 0;

    // Counters captured via Leg::record_counters.
    std::vector<std::pair<std::string, std::uint64_t>> counters;
};

// Collects named counters from a leg.
class ResultRecorder {
public:
    void count(std::string name, std::uint64_t value) {
        entries_.emplace_back(std::move(name), value);
    }
    [[nodiscard]] const std::vector<std::pair<std::string, std::uint64_t>>& entries() const {
        return entries_;
    }

private:
    std::vector<std::pair<std::string, std::uint64_t>> entries_;
};

struct RunOptions {
    // S57 asks for at least 20. Fewer makes the median unstable on a machine
    // whose timings are this noisy.
    int repetitions = 20;

    // Discarded. The first runs of any workload pay page faults, branch
    // mispredictions and a cold cache that the steady state does not.
    int warmup = 3;

    // Between samples, so heat from one leg does not land on the next.
    std::chrono::milliseconds cooldown{25};

    // A sample whose observed clock falls below this fraction of the nominal
    // maximum is flagged as throttled. 0.80 rather than something tighter because
    // a mobile part rarely sits at its nominal peak even when healthy.
    double throttle_fraction = 0.80;
};

struct RunReport {
    std::vector<LegResult> legs;
    RunOptions             options;
    double                 nominal_max_mhz = 0.0;

    // True when frequency could not be read at all, in which case no claim about
    // throttling should be made in either direction.
    bool clock_unavailable = false;
};

// Runs every leg `repetitions` times, interleaved, and summarises.
[[nodiscard]] RunReport run_interleaved(const std::vector<Leg>& legs, const RunOptions& options);

// Times a single callable, in seconds, using the steady clock.
template <typename F>
[[nodiscard]] double time_seconds(F&& body) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto finish = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(finish - start).count();
}

} // namespace thunderbolt::bench

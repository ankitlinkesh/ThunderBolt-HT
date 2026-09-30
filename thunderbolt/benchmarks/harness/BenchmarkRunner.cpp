#include "BenchmarkRunner.hpp"

#include <algorithm>
#include <numeric>
#include <random>
#include <thread>

namespace thunderbolt::bench {
namespace {

void cooldown_for(std::chrono::milliseconds duration) {
    if (duration.count() > 0) {
        std::this_thread::sleep_for(duration);
    }
}

} // namespace

const char* to_string(LegOrder order) {
    switch (order) {
        case LegOrder::Fixed:   return "fixed";
        case LegOrder::Reverse: return "reverse";
        case LegOrder::Shuffle: return "shuffle";
    }
    return "?";
}

bool parse_leg_order(std::string_view text, LegOrder& out) {
    for (LegOrder candidate : {LegOrder::Fixed, LegOrder::Reverse, LegOrder::Shuffle}) {
        if (text == to_string(candidate)) {
            out = candidate;
            return true;
        }
    }
    return false;
}

RunReport run_interleaved(const std::vector<Leg>& legs, const RunOptions& options) {
    RunReport report;
    report.options         = options;
    report.nominal_max_mhz = nominal_max_mhz();
    report.clock_unavailable = (report.nominal_max_mhz <= 0.0);

    report.legs.reserve(legs.size());
    for (const Leg& leg : legs) {
        LegResult result;
        result.name = leg.name;
        report.legs.push_back(std::move(result));
    }

    const int total_rounds = options.warmup + options.repetitions;

    // The interleaving lives here: the OUTER loop is the repetition and the INNER
    // loop is the leg, so the schedule is A B A B rather than AAAA BBBB. Reversing
    // these two loops would silently reintroduce the drift bias this whole harness
    // exists to avoid.
    //
    // The ORDER of the inner loop is not fixed: a fixed order gives each leg a
    // permanent slot and predecessor, so a position effect cannot be told apart
    // from a leg effect. Measured: taskflow_explicit read 836 ns/task directly
    // after taskflow_for_each and 319 in its usual slot. Results are still stored
    // by leg index, so callers indexing report.legs are unaffected.
    std::mt19937_64 rng(options.order_seed);
    std::vector<std::size_t> order(legs.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    if (options.leg_order == LegOrder::Reverse) {
        std::reverse(order.begin(), order.end());
    }

    for (int round = 0; round < total_rounds; ++round) {
        const bool is_warmup = round < options.warmup;

        if (options.leg_order == LegOrder::Shuffle) {
            // Hand-rolled Fisher-Yates: std::shuffle's result is implementation
            // defined, and the recorded seed must reproduce the same schedule.
            for (std::size_t k = order.size(); k > 1; --k) {
                std::swap(order[k - 1], order[static_cast<std::size_t>(rng() % k)]);
            }
        }

        for (std::size_t slot = 0; slot < order.size(); ++slot) {
            const std::size_t i = order[slot];
            cooldown_for(options.cooldown);

            const double mhz_before = sample_current_mhz();
            const double seconds    = legs[i].run_once();
            const double mhz_after  = sample_current_mhz();

            Sample sample;
            sample.seconds = seconds;
            sample.warmup  = is_warmup;
            sample.position = static_cast<int>(slot);

            if (mhz_before > 0.0 && mhz_after > 0.0) {
                sample.clock_mhz = 0.5 * (mhz_before + mhz_after);
                // Flagged in a second pass, once the run's own median clock is
                // known - see below.
            }

            if (!is_warmup) {
                report.legs[i].samples.push_back(sample);
            }
        }
    }

    // Throttle flagging, relative to THIS RUN's median clock rather than to the
    // nominal maximum.
    //
    // The absolute comparison was wrong, and measuring exposed it: on the
    // reference machine every sustained multicore sample sits near 1600 MHz
    // against a 2208 MHz nominal maximum, so a fixed 80%-of-max floor flagged
    // 100% of samples and told a reader nothing. That is not throttling - it is
    // simply the all-core clock a 15 W part can hold.
    //
    // What actually biases an A/B is a sample that ran materially slower than its
    // PEERS, which is what a drifting clock produces. So the reference is the
    // median observed clock across the whole interleaved run, and a sample well
    // below it is the anomaly worth excluding.
    std::vector<double> observed_clocks;
    for (const LegResult& leg : report.legs) {
        for (const Sample& sample : leg.samples) {
            if (sample.clock_mhz > 0.0) {
                observed_clocks.push_back(sample.clock_mhz);
            }
        }
    }
    const double median_clock =
        observed_clocks.empty() ? 0.0 : summarize(observed_clocks).median;
    report.median_observed_mhz = median_clock;

    const double throttle_floor = median_clock * options.throttle_fraction;
    for (LegResult& leg : report.legs) {
        for (Sample& sample : leg.samples) {
            sample.throttled = (sample.clock_mhz > 0.0) && (sample.clock_mhz < throttle_floor);
        }
    }

    for (std::size_t i = 0; i < legs.size(); ++i) {
        LegResult& result = report.legs[i];

        std::vector<double> clean;
        std::vector<double> all;
        for (const Sample& sample : result.samples) {
            all.push_back(sample.seconds);
            if (sample.throttled) {
                ++result.throttled_count;
            } else {
                clean.push_back(sample.seconds);
            }
        }

        result.timing_including_throttled = summarize(all);

        // If throttling removed so many samples that nothing meaningful is left,
        // report the full set rather than a median of two points - and the
        // throttled_count in the output is what tells a reader to distrust it.
        result.timing = (clean.size() >= 3) ? summarize(clean) : result.timing_including_throttled;

        if (legs[i].record_counters) {
            ResultRecorder recorder;
            legs[i].record_counters(recorder);
            result.counters = recorder.entries();
        }
    }

    return report;
}

} // namespace thunderbolt::bench

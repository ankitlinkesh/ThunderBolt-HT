// Thunderbolt HT - scheduling strategies (S12).
//
// Each mode is an EXPERIMENT, not a feature. S95.12 requires every one to ship
// with a benchmark saying whether it beat the previous mode, and modes that lose
// are kept and documented as negative results rather than quietly deleted -
// "static scheduling was tried and lost" answers S59.2, which is the question.
#pragma once

#include <cstdint>

namespace thunderbolt {

enum class SchedulerMode : std::uint8_t {
    // S12 Mode 1. Tasks are assigned to workers round-robin at submission and a
    // worker only ever runs its own queue. No stealing.
    //
    // The control for S59.2 ("how does work stealing compare with static
    // scheduling for mixed game workloads?"). It cannot be answered by reasoning:
    // static wins when the work is uniform, because it never touches another
    // core's cache line, and loses when it is not. Only measurement says which a
    // frame graph is.
    Static = 0,

    // S12 Mode 2, and the default. Per-worker deques with randomised stealing.
    // Strict priority order: highest non-empty priority always wins.
    WorkStealing = 1,

    // S12 Mode 3. Work stealing, plus aging so that low-priority work cannot be
    // starved indefinitely by a steady stream of higher-priority work (S13).
    //
    // Deliberately NOT the default. Aging necessarily weakens strict priority -
    // that is what it is for - and which trade is right depends on the workload,
    // so it is a choice the caller makes rather than one taken on their behalf.
    PriorityAging = 2,
};

// One pop in every kAgingInterval takes from the LOWEST non-empty priority
// instead of the highest.
//
// Chosen over a timestamp-based scheme because it needs no clock read on the pop
// path and gives a bound that can actually be asserted: background work receives
// at least one pop in kAgingInterval, so its wait is bounded by a task count
// rather than by "eventually". The starvation test checks exactly that bound.
//
// 8 costs strict priority about 12% of throughput at the top end. That is a real
// price, which is why aging is opt-in.
inline constexpr std::uint64_t kAgingInterval = 8;

// True when this pop should scan priorities in reverse. Pure, so both runtimes
// share one definition of the policy and cannot drift apart on it.
[[nodiscard]] inline constexpr bool aging_pop_favours_lowest(SchedulerMode mode,
                                                             std::uint64_t pop_index) noexcept {
    return mode == SchedulerMode::PriorityAging &&
           (pop_index % kAgingInterval) == (kAgingInterval - 1);
}

[[nodiscard]] inline constexpr const char* scheduler_mode_name(SchedulerMode mode) noexcept {
    switch (mode) {
    case SchedulerMode::Static:        return "static";
    case SchedulerMode::WorkStealing:  return "stealing";
    case SchedulerMode::PriorityAging: return "priority_aging";
    }
    return "unknown";
}

} // namespace thunderbolt

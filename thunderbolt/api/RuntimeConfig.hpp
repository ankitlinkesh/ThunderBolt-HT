// Thunderbolt HT - runtime construction parameters.
//
// Shared by every ITaskRuntime implementation on purpose: a knob that exists for
// one runtime and not the other is a knob that cannot be held constant across an
// A/B comparison (S57, S94).
#pragma once

#include <thunderbolt/api/SchedulerMode.hpp>

#include <cstdint>

namespace thunderbolt {

enum class AffinityMode : std::uint8_t {
    // Let the OS place threads. The default, and not a lazy one: S17 warns that
    // naive pinning can lose to the OS scheduler, and on a 15 W part the OS
    // migrates threads partly to spread heat - so pinning can cost more in
    // throttling than it recovers in cache locality. It must be beaten by
    // measurement before anything else is chosen.
    Disabled = 0,

    // Pin worker N to a logical processor from the detected topology, filling
    // distinct physical cores before doubling up on SMT siblings.
    TopologyAware = 1,
};

// Optimisations under ablation.
//
// Each is OFF by default so the baseline is untouched, and each is selectable as
// a separate BENCHMARK LEG. That matters: an ablation compared across separate
// runs is the batched-comparison bias this project has already been bitten by
// twice. Both code paths must live in one binary so they can be interleaved.
//
// A flag that earns its place becomes unconditional and its flag is deleted. One
// that does not is reverted, like the task-padding probe before it.
enum RuntimeOpt : std::uint32_t {
    kOptNone = 0,

    // Skip the fence-bearing deque pop for a deque that a relaxed probe already
    // says is empty.
    //
    // AbpDeque::pop() issues a seq_cst fence BEFORE it discovers the deque is
    // empty, and pop_local scans up to five priority deques. With work usually in
    // one priority level that is up to four wasted full barriers per pop. The
    // probe is safe because the owner is the only pusher, so its own view of
    // `bottom` is authoritative, and `top` only ever increases - a stale read can
    // only make the deque look FULLER, which falls through to the correct path.
    kOptSkipEmptyDeques = 1u << 0,

    // Drop the standalone seq_cst fence in complete() and promote the adjacent
    // outstanding_ decrement to seq_cst instead. A locked read-modify-write is
    // already a full barrier, so the separate fence buys nothing on x86 - and
    // making the RMW seq_cst keeps the ordering correct on weak memory models
    // rather than relying on x86's stronger guarantees.
    kOptSingleBarrierOnComplete = 1u << 1,
};

struct RuntimeConfig {
    // Worker threads to create. 0 means "one per logical processor".
    //
    // Note that S58's scaling sweep depends on this being honoured exactly:
    // a runtime that quietly adds a helper thread would report speedups for a
    // worker count it is not actually running.
    std::uint32_t worker_count = 0;

    // Task pool size. Fixed rather than growable, so that submission never
    // reaches the allocator on the hot path. Exhaustion degrades to inline
    // execution and is counted, rather than failing or silently allocating.
    std::uint32_t task_capacity = 65536;

    // Per-worker, per-priority work-stealing deque capacity.
    //
    // Matters for fork-join bursts: a task that spawns thousands of children puts
    // them all on ITS OWN deque before any are stolen, and overflow spills to the
    // global queue - which is exactly the contended path the deques exist to
    // avoid. Overflow is counted, so a capacity set too low is visible rather
    // than silently changing what the benchmark measures.
    std::uint32_t deque_capacity = 4096;

    // S12. Which scheduling strategy ThunderboltRuntime uses. Ignored by
    // StandardRuntime, which has one strategy by definition - it is the baseline.
    SchedulerMode scheduler = SchedulerMode::WorkStealing;

    // Bitwise OR of RuntimeOpt. Zero is the shipped behaviour.
    std::uint32_t optimizations = kOptNone;

    // Off by default (S17). Lives here rather than on one runtime so that it
    // stays constant across an A/B comparison instead of being one runtime's
    // private advantage.
    AffinityMode affinity = AffinityMode::Disabled;
};

} // namespace thunderbolt

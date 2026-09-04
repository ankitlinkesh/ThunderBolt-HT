// Thunderbolt HT - runtime construction parameters.
//
// Shared by every ITaskRuntime implementation on purpose: a knob that exists for
// one runtime and not the other is a knob that cannot be held constant across an
// A/B comparison (S57, S94).
#pragma once

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

    // Off by default (S17). Lives here rather than on one runtime so that it
    // stays constant across an A/B comparison instead of being one runtime's
    // private advantage.
    AffinityMode affinity = AffinityMode::Disabled;
};

} // namespace thunderbolt

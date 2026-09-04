// Thunderbolt HT - runtime construction parameters.
//
// Shared by every ITaskRuntime implementation on purpose: a knob that exists for
// one runtime and not the other is a knob that cannot be held constant across an
// A/B comparison (S57, S94).
#pragma once

#include <cstdint>

namespace thunderbolt {

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
};

} // namespace thunderbolt

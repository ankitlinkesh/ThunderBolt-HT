// Thunderbolt HT - fundamental task vocabulary.
//
// Kept in api/ because application code legitimately names these. Runtime
// internals (workers, deques, schedulers) never appear in this directory: S66
// draws the line here.
#pragma once

#include <cstddef>
#include <cstdint>

namespace thunderbolt {

// Cache line size. Used to keep per-task mutable state off shared lines, which
// is the difference between "parallel" and "parallel but false-sharing".
// Not std::hardware_destructive_interference_size: that is a compile-time
// constant baked into ABI decisions, and pinning it makes layout reproducible
// across toolchains, which the determinism work depends on.
inline constexpr std::size_t kCacheLineSize = 64;

// S13. Ordering matters: lower numeric value == more urgent, so priority
// comparisons are plain integer comparisons on the hot path.
enum class TaskPriority : std::uint8_t {
    Critical   = 0,  // player physics, immediate collision, frame-critical work
    High       = 1,  // nearby vehicles and NPCs, aircraft physics
    Normal     = 2,  // ordinary world simulation, animation
    Low        = 3,  // distant NPCs and traffic
    Background = 4,  // prefetch, asset preparation, statistical simulation
    Count      = 5
};

inline constexpr std::size_t kPriorityCount = static_cast<std::size_t>(TaskPriority::Count);

// S6. Reserved for scheduling hints that must not change program meaning - a
// flag may make execution faster or slower, never incorrect.
enum class TaskFlags : std::uint32_t {
    None = 0,
};

// S7 task lifecycle. Stored as an atomic on the task; the transitions that
// matter for correctness are Queued -> Executing -> Completed.
//
// Free is not part of S7's list: it distinguishes a pool slot that is available
// for reuse from one holding a completed task, which is what makes stale-handle
// detection possible rather than undefined behaviour (S62).
enum class TaskState : std::uint32_t {
    Free      = 0,
    Created   = 1,
    Waiting   = 2,  // dependencies outstanding (Phase D)
    Ready     = 3,
    Queued    = 4,
    Claimed   = 5,
    Executing = 6,
    Completed = 7,
};

} // namespace thunderbolt

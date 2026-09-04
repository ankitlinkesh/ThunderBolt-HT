// Thunderbolt HT - task submission description.
#pragma once

#include <thunderbolt/api/TaskFunction.hpp>
#include <thunderbolt/api/TaskTypes.hpp>

#include <cstdint>

namespace thunderbolt {

// Everything the runtime needs in order to schedule one task (S6).
//
// Move-only, because it owns the task body. Fields beyond the body are hints:
// a scheduler is free to ignore every one of them and still be correct, which
// is what makes it safe to compare scheduling strategies on identical workloads.
struct TaskDesc {
    TaskFunction function;

    TaskPriority priority = TaskPriority::Normal;
    TaskFlags    flags    = TaskFlags::None;

    // S14. Caller's estimate in arbitrary units, 0 meaning "no estimate".
    // Phase G replaces the caller's guess with measured history where the
    // measurement proves worthwhile; until then this is recorded, not trusted.
    std::uint32_t estimated_cost = 0;

    TaskDesc() = default;

    // Implicit so that submit() can take a bare lambda.
    TaskDesc(TaskFunction fn) : function(std::move(fn)) {}  // NOLINT(google-explicit-constructor)

    TaskDesc(TaskFunction fn, TaskPriority prio)
        : function(std::move(fn)), priority(prio) {}
};

} // namespace thunderbolt

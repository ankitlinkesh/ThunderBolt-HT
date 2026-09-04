// Thunderbolt HT - execution context handed to a running task.
#pragma once

#include <cstdint>

namespace thunderbolt {

class ITaskRuntime;

// Passed by reference to every task body.
//
// Carrying the runtime here (rather than expecting tasks to capture it) is what
// lets a task submit further work and wait on it without knowing which runtime
// it is running under - the property S65/S66 depend on.
struct TaskContext {
    // Index of the worker executing this task, in [0, worker_count).
    // kExternalThread when a task is executed inline by a non-worker thread,
    // which happens when an outside caller helps during wait().
    static constexpr std::uint32_t kExternalThread = 0xFFFFFFFFu;

    ITaskRuntime* runtime      = nullptr;
    std::uint32_t worker_index = kExternalThread;

    [[nodiscard]] constexpr bool on_worker_thread() const noexcept {
        return worker_index != kExternalThread;
    }
};

} // namespace thunderbolt

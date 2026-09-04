// Thunderbolt HT - task identity.
#pragma once

#include <cstdint>

namespace thunderbolt {

// A reference to a submitted task.
//
// Deliberately NOT a pointer. Tasks live in a recycled pool, so a raw pointer to
// a completed task is indistinguishable from a pointer to whatever task later
// occupies that slot - exactly the "invalid task handle" and "use-after-free"
// failure modes S62 requires the runtime to detect. Pairing the slot index with
// a generation counter that increments on every reuse turns both into a cheap
// equality check instead of undefined behaviour.
//
// Trivially copyable and 64 bits wide, so passing one costs a register.
struct TaskHandle {
    static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

    std::uint32_t index      = kInvalidIndex;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return index != kInvalidIndex; }

    [[nodiscard]] friend constexpr bool operator==(TaskHandle a, TaskHandle b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
    [[nodiscard]] friend constexpr bool operator!=(TaskHandle a, TaskHandle b) noexcept {
        return !(a == b);
    }
};

static_assert(sizeof(TaskHandle) == 8, "TaskHandle is passed by value on hot paths");

} // namespace thunderbolt

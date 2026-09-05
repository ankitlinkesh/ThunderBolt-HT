#include <thunderbolt/core/ShardedCounter.hpp>

namespace thunderbolt {
namespace {

// Handed out round-robin on first use. A thread-id hash would also work but
// distributes unpredictably; a counter keeps distinct threads on distinct slots
// for as many threads as there are slots, which is the case that matters.
std::atomic<std::uint32_t> g_next_slot{0};

} // namespace

std::uint32_t thread_slot() noexcept {
    static thread_local const std::uint32_t slot =
        g_next_slot.fetch_add(1, std::memory_order_relaxed) % kCounterShards;
    return slot;
}

} // namespace thunderbolt

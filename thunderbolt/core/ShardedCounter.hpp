// Thunderbolt HT - a counter that is cheap to write and rare to read.
//
// WHY THIS EXISTS. Five global atomic read-modify-writes ran on every task, and
// the counters holding them shared one 64-byte cache line. That line was written
// twice per task by every worker, so it ping-ponged across all eight cores -
// true sharing and false sharing at the same address. Estimated at 150-470 ns per
// task against a measured 391 ns gap to Taskflow, it was the first hypothesis
// whose magnitude actually covered the shortfall.
//
// Four of those five counters exist only to be REPORTED - task counts, pool
// acquire/release counts, contention tallies. Nothing on the hot path reads them.
// So each write goes to a per-thread slot on its own cache line, and the total is
// computed by summing when somebody asks.
//
// The exception is deliberately NOT sharded: `outstanding_` in RuntimeBase, which
// wait_all() needs an exact zero from. Summing sixteen slots that other cores are
// actively writing means sixteen cache misses - worse than the single atomic it
// would replace. It stays one atomic, on its own cache line.
#pragma once

#include <thunderbolt/api/TaskTypes.hpp>

#include <atomic>
#include <cstdint>

namespace thunderbolt {

// A stable per-thread index, shared by every sharded structure in the runtime.
//
// One scheme rather than several: the task pool already had its own thread-local
// shard selector, and a second independent one would put the same thread on
// different slots in different structures for no reason.
[[nodiscard]] std::uint32_t thread_slot() noexcept;

// Number of shards. Above the reference machine's logical processor count so
// distinct threads rarely collide, and a power of two so selection is a mask.
inline constexpr std::uint32_t kCounterShards = 16;

// Write-mostly counter. `add` touches one cache line owned by the calling thread;
// `sum` walks all of them and is intended for report and wait paths only.
class ShardedCounter {
public:
    void add(std::int64_t delta) noexcept {
        // Relaxed: these are statistics, and no algorithm branches on them. An
        // ordering guarantee here would cost exactly what this class exists to
        // remove.
        slots_[thread_slot()].value.fetch_add(delta, std::memory_order_relaxed);
    }

    void increment() noexcept { add(1); }

    [[nodiscard]] std::int64_t sum() const noexcept {
        std::int64_t total = 0;
        for (const Slot& slot : slots_) {
            total += slot.value.load(std::memory_order_relaxed);
        }
        return total;
    }

    // Unsigned view, for counters that cannot legitimately go negative. Clamps
    // rather than wrapping, so a torn read during concurrent updates reports zero
    // instead of an absurd number.
    [[nodiscard]] std::uint64_t unsigned_sum() const noexcept {
        const std::int64_t total = sum();
        return (total > 0) ? static_cast<std::uint64_t>(total) : 0;
    }

private:
    // One slot per cache line. Without the alignment this class would reproduce
    // the false sharing it was written to eliminate.
    TB_BEGIN_CACHE_ALIGNED_TYPE
    struct alignas(kCacheLineSize) Slot {
        std::atomic<std::int64_t> value{0};
    };
    TB_END_CACHE_ALIGNED_TYPE

    Slot slots_[kCounterShards];
};

} // namespace thunderbolt

// Thunderbolt HT - synthetic workloads for the runtime benchmarks.
//
// Properties every workload here must have, and why:
//
// - INTEGER ONLY. No floating point, so results do not depend on FP evaluation
//   order and a run is bit-identical regardless of how it was scheduled.
// - NO ALLOCATION inside the timed region. A workload that touches the allocator
//   measures allocator contention and reports it as scheduler overhead.
// - NO SHARED WRITES. Each chunk writes to its own output slot, spaced so two
//   chunks never share a cache line. Without that spacing the benchmark measures
//   false sharing, which looks exactly like poor scheduling.
// - COMPILER-RESISTANT. The result is written out, so the loop cannot be
//   optimised away - a benchmark whose body is eliminated measures the scheduler
//   dispatching empty work.
#pragma once

#include <thunderbolt/api/TaskTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace thunderbolt::bench {

// One unit of ALU-bound work: the murmur3 finaliser. Chosen because it is a few
// multiplies and shifts with a serial dependency chain, so it cannot be
// vectorised away and its cost per call is stable and predictable.
[[nodiscard]] inline std::uint64_t mix(std::uint64_t x) noexcept {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// Accumulates `count` work units starting from `start`. Serial by construction.
[[nodiscard]] inline std::uint64_t work_range(std::uint64_t start, std::uint64_t count) noexcept {
    std::uint64_t accumulator = start * 0x9E3779B97F4A7C15ULL;
    for (std::uint64_t i = 0; i < count; ++i) {
        accumulator = mix(accumulator ^ (start + i));
    }
    return accumulator;
}

// Per-chunk output, padded so that two chunks never share a cache line.
struct alignas(kCacheLineSize) PaddedResult {
    std::uint64_t value = 0;
    std::uint8_t  padding[kCacheLineSize - sizeof(std::uint64_t)]{};
};

static_assert(sizeof(PaddedResult) == kCacheLineSize,
              "chunk results must occupy a whole cache line, or the benchmark measures false "
              "sharing rather than scheduling");

// Storage for a chunked workload. Allocated once, outside every timed region.
class ChunkedWorkload {
public:
    ChunkedWorkload(std::uint64_t total_units, std::size_t chunk_count)
        : total_units_(total_units), chunk_count_(chunk_count), results_(chunk_count) {}

    [[nodiscard]] std::size_t   chunk_count() const noexcept { return chunk_count_; }
    [[nodiscard]] std::uint64_t total_units() const noexcept { return total_units_; }

    // Units handled by chunk `index`. The last chunk absorbs the remainder, so the
    // TOTAL work is identical for every chunk count - which is the whole point of
    // the granularity experiment: only the number of tasks changes.
    [[nodiscard]] std::uint64_t units_for(std::size_t index) const noexcept {
        const std::uint64_t base = total_units_ / chunk_count_;
        const std::uint64_t rest = total_units_ % chunk_count_;
        return base + ((index + 1 == chunk_count_) ? rest : 0);
    }

    [[nodiscard]] std::uint64_t start_for(std::size_t index) const noexcept {
        return (total_units_ / chunk_count_) * index;
    }

    void run_chunk(std::size_t index) noexcept {
        results_[index].value = work_range(start_for(index), units_for(index));
    }

    // Sum of every chunk result. Read after a run so the compiler cannot discard
    // the work, and comparable across chunk counts as a sanity check that the
    // same work really was performed.
    [[nodiscard]] std::uint64_t checksum() const noexcept {
        std::uint64_t total = 0;
        for (const PaddedResult& result : results_) {
            total += result.value;
        }
        return total;
    }

private:
    std::uint64_t             total_units_;
    std::size_t               chunk_count_;
    std::vector<PaddedResult> results_;
};

} // namespace thunderbolt::bench

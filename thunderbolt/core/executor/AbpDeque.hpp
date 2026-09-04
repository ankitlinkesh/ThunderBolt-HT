// Thunderbolt HT - bounded lock-free work-stealing deque.
//
// One owner pushes and pops at the bottom; any number of thieves steal from the
// top. This is the data structure the whole runtime is built on, and the one
// place where getting a memory ordering wrong produces a bug that appears once
// an hour under load and never in a test.
//
// PROVENANCE. The algorithm is Arora-Blumofe-Plaxton, in the formulation given
// by Le, Pop, Cohen and Zappa Nardelli, "Correct and Efficient Work-Stealing for
// Weak Memory Models" (PPoPP 2013). The fences below are theirs, not invented
// here: their contribution is precisely the minimal set of barriers that keeps
// the deque linearizable once sequential consistency is dropped. Chase-Lev
// deques are correct under SC and can FAIL on ARM/POWER without them.
//
// WHY BOUNDED. Chase-Lev's growable circular array is the part that breaks -
// growth races with concurrent steals and is the usual source of subtle bugs.
// This project has no ThreadSanitizer available, so the growable variant is
// deliberately deferred: a fixed capacity that reports "full" is a degradation
// the caller can handle, whereas a buggy grow path is a silent corruption. A
// machine-checked Chase-Lev proof already exists (arXiv 2309.03642) for when
// that changes.
#pragma once

#include <thunderbolt/api/TaskTypes.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace thunderbolt {

enum class StealOutcome : std::uint8_t {
    Success,  // an item was taken
    Empty,    // the victim had nothing
    Abort,    // lost a race with another thief or the owner; retry is reasonable
};

TB_BEGIN_CACHE_ALIGNED_TYPE

template <typename T>
class AbpDeque {
    static_assert(std::is_trivially_copyable_v<T>,
                  "Deque elements are read speculatively before the CAS that claims them, so a "
                  "non-trivial copy could run on an element another thread simultaneously wins.");

public:
    // `capacity` is rounded up to a power of two so that index wrapping is a
    // mask rather than a division on the hottest path in the runtime.
    explicit AbpDeque(std::size_t capacity) {
        std::size_t rounded = 1;
        while (rounded < capacity) {
            rounded <<= 1;
        }
        capacity_ = rounded;
        mask_     = static_cast<std::int64_t>(rounded - 1);
        buffer_   = std::make_unique<T[]>(rounded);
    }

    AbpDeque(const AbpDeque&)            = delete;
    AbpDeque& operator=(const AbpDeque&) = delete;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // OWNER ONLY. Returns false when the deque is full; the caller is expected to
    // route the item elsewhere rather than block or grow.
    bool push(T value) {
        const std::int64_t b = bottom_.load(std::memory_order_relaxed);
        const std::int64_t t = top_.load(std::memory_order_acquire);

        if (b - t >= static_cast<std::int64_t>(capacity_)) {
            return false;  // full
        }

        buffer_[b & mask_] = value;

        // The item must be visible to a thief before the bottom index that
        // exposes it. Without this release, a thief can observe the new bottom
        // and read a slot that still holds the previous occupant.
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
        return true;
    }

    // OWNER ONLY. Takes from the bottom - the opposite end from thieves, which is
    // what keeps the common case contention-free and is also what makes the
    // Blumofe-Leiserson time bound hold.
    bool pop(T& out) {
        const std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        bottom_.store(b, std::memory_order_relaxed);

        // Sequentially consistent, and it must be. Claiming the bottom and then
        // reading the top is a Store-Load pair; if it is reordered, the owner and
        // a thief can both conclude they won the last element.
        std::atomic_thread_fence(std::memory_order_seq_cst);

        const std::int64_t t = top_.load(std::memory_order_relaxed);

        if (t > b) {
            bottom_.store(b + 1, std::memory_order_relaxed);  // empty; restore
            return false;
        }

        out = buffer_[b & mask_];

        if (t != b) {
            return true;  // more than one item: no thief can be after this one
        }

        // Exactly one item left, so the owner is racing every thief for it.
        // Whoever advances `top` wins.
        std::int64_t expected = t;
        const bool   won      = top_.compare_exchange_strong(
            expected, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);

        bottom_.store(b + 1, std::memory_order_relaxed);  // deque is now empty either way
        return won;
    }

    // Callable by any thread other than the owner.
    StealOutcome steal(T& out) {
        const std::int64_t t = top_.load(std::memory_order_acquire);

        // Mirrors the fence in pop(): read top, then bottom, with no reordering.
        std::atomic_thread_fence(std::memory_order_seq_cst);

        const std::int64_t b = bottom_.load(std::memory_order_acquire);

        if (t >= b) {
            return StealOutcome::Empty;
        }

        // Read speculatively, then claim. If the CAS fails the value is simply
        // discarded - which is why T must be trivially copyable, since this read
        // may race with the owner overwriting the slot.
        out = buffer_[t & mask_];

        std::int64_t expected = t;
        if (!top_.compare_exchange_strong(expected, t + 1, std::memory_order_seq_cst,
                                          std::memory_order_relaxed)) {
            return StealOutcome::Abort;
        }
        return StealOutcome::Success;
    }

    // A HINT, not a synchronised value: both indices move under other threads.
    // Safe for heuristics (whom to steal from, whether to park) and never for
    // correctness.
    [[nodiscard]] std::size_t size_hint() const noexcept {
        const std::int64_t b = bottom_.load(std::memory_order_relaxed);
        const std::int64_t t = top_.load(std::memory_order_relaxed);
        const std::int64_t n = b - t;
        return n > 0 ? static_cast<std::size_t>(n) : 0;
    }

    [[nodiscard]] bool empty_hint() const noexcept { return size_hint() == 0; }

private:
    // top_ and bottom_ are written by different threads on every operation.
    // Sharing a cache line would make each push contend with every steal - a
    // false-sharing stall that reads exactly like scheduler overhead.
    alignas(kCacheLineSize) std::atomic<std::int64_t> top_{0};
    alignas(kCacheLineSize) std::atomic<std::int64_t> bottom_{0};

    alignas(kCacheLineSize) std::unique_ptr<T[]> buffer_;
    std::size_t  capacity_ = 0;
    std::int64_t mask_     = 0;
};

TB_END_CACHE_ALIGNED_TYPE

} // namespace thunderbolt

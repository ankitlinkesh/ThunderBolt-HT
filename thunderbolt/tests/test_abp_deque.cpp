#include "TestHarness.hpp"

#include <thunderbolt/core/executor/AbpDeque.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <unordered_set>
#include <vector>

using thunderbolt::AbpDeque;
using thunderbolt::StealOutcome;

TB_TEST("deque rounds capacity up to a power of two") {
    AbpDeque<int> deque(100);
    TB_CHECK_EQ(deque.capacity(), 128u);
}

TB_TEST("owner push and pop are LIFO") {
    // The owner works at the bottom as a stack. That is not an arbitrary choice:
    // popping the most recently pushed task keeps a task's children on the core
    // that produced them, which is where the cache locality comes from.
    AbpDeque<int> deque(16);
    TB_CHECK(deque.push(1));
    TB_CHECK(deque.push(2));
    TB_CHECK(deque.push(3));

    int value = 0;
    TB_CHECK(deque.pop(value));
    TB_CHECK_EQ(value, 3);
    TB_CHECK(deque.pop(value));
    TB_CHECK_EQ(value, 2);
    TB_CHECK(deque.pop(value));
    TB_CHECK_EQ(value, 1);
    TB_CHECK(!deque.pop(value));
}

TB_TEST("thieves take from the opposite end, FIFO") {
    // Stealing from the top takes the OLDEST task - the one whose children are
    // least likely to still be warm in the owner's cache, and the one most likely
    // to represent a large remaining subtree.
    AbpDeque<int> deque(16);
    deque.push(1);
    deque.push(2);
    deque.push(3);

    int value = 0;
    TB_CHECK(deque.steal(value) == StealOutcome::Success);
    TB_CHECK_EQ(value, 1);
    TB_CHECK(deque.steal(value) == StealOutcome::Success);
    TB_CHECK_EQ(value, 2);
}

TB_TEST("an empty deque reports empty rather than aborting") {
    AbpDeque<int> deque(8);
    int           value = 0;
    TB_CHECK(!deque.pop(value));
    TB_CHECK(deque.steal(value) == StealOutcome::Empty);
}

TB_TEST("a full deque refuses the push instead of growing") {
    // Bounded by design. The caller routes overflow elsewhere; growing here is
    // exactly the Chase-Lev machinery this implementation deliberately avoids
    // while no ThreadSanitizer is available.
    AbpDeque<int> deque(4);
    for (int i = 0; i < 4; ++i) {
        TB_CHECK(deque.push(i));
    }
    TB_CHECK(!deque.push(99));

    int value = 0;
    TB_CHECK(deque.pop(value));
    TB_CHECK(deque.push(99));  // space freed
}

TB_TEST("indices wrap correctly across many push/pop cycles") {
    // The buffer is circular and indices are monotonic. Cycling far past capacity
    // is what catches a masking or wraparound mistake.
    AbpDeque<int> deque(8);
    for (int round = 0; round < 1000; ++round) {
        TB_CHECK(deque.push(round));
        int value = 0;
        TB_CHECK(deque.pop(value));
        TB_CHECK_EQ(value, round);
    }
    TB_CHECK_EQ(deque.size_hint(), 0u);
}

TB_TEST("owner and one thief never both win the same item") {
    // The dangerous case in every work-stealing deque: exactly one element left,
    // with the owner popping and a thief stealing at the same moment. Both must
    // not win it, and it must not vanish.
    //
    // With no ThreadSanitizer available, this is run as a long randomized race
    // rather than as a single-shot check - repetition is the substitute.
    constexpr int kRounds = 20000;

    AbpDeque<int>          deque(1024);
    std::atomic<bool>      go{false};
    std::atomic<long long> thief_total{0};
    std::atomic<int>       thief_count{0};

    std::thread thief([&] {
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        int value = 0;
        while (go.load(std::memory_order_acquire)) {
            if (deque.steal(value) == StealOutcome::Success) {
                thief_total.fetch_add(value, std::memory_order_relaxed);
                thief_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
        // Drain whatever remains after the owner stopped.
        while (deque.steal(value) == StealOutcome::Success) {
            thief_total.fetch_add(value, std::memory_order_relaxed);
            thief_count.fetch_add(1, std::memory_order_relaxed);
        }
    });

    long long owner_total = 0;
    int       owner_count = 0;

    go.store(true, std::memory_order_release);
    for (int i = 1; i <= kRounds; ++i) {
        deque.push(i);
        int value = 0;
        if (deque.pop(value)) {
            owner_total += value;
            ++owner_count;
        }
    }
    go.store(false, std::memory_order_release);
    thief.join();

    // Every pushed value was taken exactly once by exactly one party. A duplicate
    // inflates the sum; a lost item deflates it. Checking the SUM as well as the
    // count means a duplicate cannot be masked by a loss.
    const long long expected = static_cast<long long>(kRounds) * (kRounds + 1) / 2;
    TB_CHECK_EQ(owner_total + thief_total.load(), expected);
    TB_CHECK_EQ(owner_count + thief_count.load(), kRounds);
}

TB_TEST("many thieves against one owner lose nothing and duplicate nothing") {
    // Same invariant with real contention on `top`, where the CAS actually fails
    // and the Abort path is exercised.
    constexpr int kItems   = 50000;
    constexpr int kThieves = 3;

    AbpDeque<int>     deque(4096);
    std::atomic<bool> running{true};

    std::vector<std::thread>            thieves;
    std::vector<std::vector<int>>       stolen(kThieves);
    std::atomic<std::uint64_t>          aborts{0};

    for (int t = 0; t < kThieves; ++t) {
        thieves.emplace_back([&, t] {
            int value = 0;
            while (running.load(std::memory_order_acquire)) {
                const StealOutcome outcome = deque.steal(value);
                if (outcome == StealOutcome::Success) {
                    stolen[static_cast<std::size_t>(t)].push_back(value);
                } else if (outcome == StealOutcome::Abort) {
                    aborts.fetch_add(1, std::memory_order_relaxed);
                }
            }
            while (deque.steal(value) == StealOutcome::Success) {
                stolen[static_cast<std::size_t>(t)].push_back(value);
            }
        });
    }

    std::vector<int> popped;
    for (int i = 0; i < kItems; ++i) {
        // Push may fail if thieves are slower than the owner; retry so that every
        // value really does enter the deque exactly once.
        while (!deque.push(i)) {
            int value = 0;
            if (deque.pop(value)) {
                popped.push_back(value);
            }
        }
        int value = 0;
        if ((i % 3) == 0 && deque.pop(value)) {
            popped.push_back(value);
        }
    }

    // Drain the owner's side before signalling the thieves to stop.
    int value = 0;
    while (deque.pop(value)) {
        popped.push_back(value);
    }
    running.store(false, std::memory_order_release);
    for (std::thread& th : thieves) {
        th.join();
    }

    std::unordered_set<int> seen;
    for (int v : popped) {
        TB_CHECK(seen.insert(v).second);  // no value taken twice
    }
    for (const auto& per_thief : stolen) {
        for (int v : per_thief) {
            TB_CHECK(seen.insert(v).second);
        }
    }
    TB_CHECK_EQ(seen.size(), static_cast<std::size_t>(kItems));  // and none lost
}

TB_TEST("size_hint tracks the item count for a single thread") {
    AbpDeque<int> deque(64);
    TB_CHECK(deque.empty_hint());
    deque.push(1);
    deque.push(2);
    TB_CHECK_EQ(deque.size_hint(), 2u);
    int value = 0;
    (void)deque.pop(value);
    TB_CHECK_EQ(deque.size_hint(), 1u);
}

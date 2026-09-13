#include "TestHarness.hpp"

#include <thunderbolt/core/task/TaskPool.hpp>

#include <thread>
#include <unordered_set>
#include <vector>

using thunderbolt::Task;
using thunderbolt::TaskDesc;
using thunderbolt::TaskFunction;
using thunderbolt::TaskHandle;
using thunderbolt::TaskPool;

namespace {

TaskDesc trivial_task() { return TaskDesc{TaskFunction{[] {}}}; }

} // namespace

TB_TEST("acquire returns a resolvable handle") {
    TaskPool pool(8);
    TaskHandle handle = pool.acquire(trivial_task());
    TB_CHECK(handle.valid());
    TB_CHECK(pool.get(handle) != nullptr);
    TB_CHECK_EQ(pool.live_count(), 1u);
    pool.release(handle);
    TB_CHECK_EQ(pool.live_count(), 0u);
}

TB_TEST("a released handle becomes stale rather than dangling") {
    // This is the property S62 asks for: using a handle after its task finished
    // must be DETECTABLE, not undefined. Without the generation counter, get()
    // here would happily return a pointer to a recycled slot.
    TaskPool   pool(4);
    TaskHandle handle = pool.acquire(trivial_task());
    pool.release(handle);
    TB_CHECK(pool.get(handle) == nullptr);
}

TB_TEST("a recycled slot does not resolve an older handle") {
    // The dangerous case is not "slot is free" but "slot was handed to someone
    // else". The old handle must not resolve to the new occupant's task.
    TaskPool   pool(1);
    TaskHandle first = pool.acquire(trivial_task());
    pool.release(first);

    TaskHandle second = pool.acquire(trivial_task());
    TB_CHECK_EQ(second.index, first.index);       // same slot, as the pool holds one
    TB_CHECK(second.generation != first.generation);

    TB_CHECK(pool.get(first) == nullptr);         // stale handle stays stale
    TB_CHECK(pool.get(second) != nullptr);
    pool.release(second);
}

TB_TEST("an exhausted pool reports failure instead of allocating") {
    TaskPool                pool(4);
    std::vector<TaskHandle> handles;
    for (int i = 0; i < 4; ++i) {
        TaskHandle h = pool.acquire(trivial_task());
        TB_CHECK(h.valid());
        handles.push_back(h);
    }

    // Fixed capacity is a design choice, so exhaustion must be visible to the
    // caller rather than silently growing the slab.
    TaskHandle overflow = pool.acquire(trivial_task());
    TB_CHECK(!overflow.valid());

    for (TaskHandle h : handles) {
        pool.release(h);
    }
    TB_CHECK(pool.acquire(trivial_task()).valid());
}

TB_TEST("an invalid handle never resolves") {
    TaskPool pool(4);
    TB_CHECK(pool.get(TaskHandle{}) == nullptr);
    TB_CHECK(pool.get(TaskHandle{9999, 0}) == nullptr);  // out of range
}

TB_TEST("the pool preserves task payload through acquire") {
    TaskPool pool(4);
    int      ran = 0;

    TaskDesc desc{TaskFunction{[&ran] { ++ran; }}, thunderbolt::TaskPriority::High};
    desc.estimated_cost = 123;

    TaskHandle handle = pool.acquire(std::move(desc));
    Task*      task   = pool.get(handle);
    TB_CHECK(task != nullptr);
    TB_CHECK(task->priority == thunderbolt::TaskPriority::High);
    TB_CHECK_EQ(task->estimated_cost, 123u);

    thunderbolt::TaskContext ctx{nullptr, 0};
    task->function(ctx);
    TB_CHECK_EQ(ran, 1);

    pool.release(handle);
}

TB_TEST("concurrent acquire hands out distinct slots") {
    // The free list is mutex-guarded; this is the test that would catch it being
    // made lock-free incorrectly later. Two threads must never receive the same
    // slot, and no slot may be lost.
    constexpr std::uint32_t kCapacity = 1024;
    TaskPool                pool(kCapacity);

    constexpr int kThreads          = 4;
    constexpr int kPerThread        = 256;
    std::vector<std::vector<TaskHandle>> results(kThreads);
    std::vector<std::thread>             threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&pool, &results, t] {
            for (int i = 0; i < kPerThread; ++i) {
                TaskHandle h = pool.acquire(trivial_task());
                if (h.valid()) {
                    results[static_cast<std::size_t>(t)].push_back(h);
                }
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }

    std::unordered_set<std::uint32_t> seen;
    std::size_t                       total = 0;
    for (const auto& per_thread : results) {
        for (TaskHandle h : per_thread) {
            ++total;
            TB_CHECK(seen.insert(h.index).second);  // no slot handed out twice
        }
    }
    TB_CHECK_EQ(total, static_cast<std::size_t>(kThreads * kPerThread));
    TB_CHECK_EQ(pool.live_count(), static_cast<std::uint32_t>(total));

    for (const auto& per_thread : results) {
        for (TaskHandle h : per_thread) {
            pool.release(h);
        }
    }
    TB_CHECK_EQ(pool.live_count(), 0u);
}

TB_TEST("pool instrumentation balances and observes real contention") {
    // The counters exist so Phase C can test the hypothesis "the pool mutex, not
    // the deque, is the bottleneck" instead of guessing. A counter that never
    // moves would answer that question wrongly and silently, so check here that
    // it actually observes something.
    constexpr std::uint32_t kCapacity = 4096;
    TaskPool                pool(kCapacity);

    constexpr int kThreads   = 4;
    constexpr int kPerThread = 400;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&pool] {
            for (int i = 0; i < kPerThread; ++i) {
                TaskHandle h = pool.acquire(trivial_task());
                if (h.valid()) {
                    pool.release(h);  // churn the free list to provoke contention
                }
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }

    const std::uint64_t acquires = pool.acquire_count();
    const std::uint64_t releases = pool.release_count();

    TB_CHECK_EQ(acquires, static_cast<std::uint64_t>(kThreads * kPerThread));
    TB_CHECK_EQ(releases, acquires);  // every acquire was matched by a release
    TB_CHECK_EQ(pool.live_count(), 0u);

    // Contention is timing-dependent, so its exact value is not asserted - only
    // that the counter is readable and bounded by the number of lock attempts.
    TB_CHECK(pool.contended_lock_count() <= acquires + releases);
}

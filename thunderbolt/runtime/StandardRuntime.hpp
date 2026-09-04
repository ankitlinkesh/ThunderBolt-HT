// Thunderbolt HT - the baseline runtime.
//
// S11: this is a BASELINE, not a straw man. Beating a deliberately poor
// implementation would prove nothing, so this is the design a competent engineer
// reaches for without a research agenda: long-lived worker threads, priority FIFO
// queues under a mutex, condition-variable parking, no busy-waiting. Where a
// cheap improvement exists that any reasonable implementation would include, it
// is included.
//
// Everything that is NOT work distribution - task lifetime, completion, waiting,
// help-on-wait, pool exhaustion - comes from RuntimeBase and is therefore
// byte-for-byte the same code ThunderboltRuntime runs. That is what makes the
// two comparable.
#pragma once

#include <thunderbolt/runtime/RuntimeBase.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace thunderbolt {

class StandardRuntime final : public RuntimeBase {
public:
    explicit StandardRuntime(RuntimeConfig config = {});
    ~StandardRuntime() override;

    // Bring the submit(callable) convenience overload back into scope; declaring
    // submit(TaskDesc) below would otherwise hide it.
    using ITaskRuntime::submit;

    [[nodiscard]] TaskHandle submit(TaskDesc desc) override;

    [[nodiscard]] std::uint32_t    worker_count() const noexcept override { return worker_count_; }
    [[nodiscard]] std::string_view name() const noexcept override { return "standard"; }

    // Workers whose affinity request the OS actually honoured. A refused request
    // must not be reported as a pinned run.
    [[nodiscard]] std::uint32_t pinned_worker_count() const noexcept {
        return pinned_workers_.load(std::memory_order_relaxed);
    }

protected:
    bool               try_execute_one(TaskContext& ctx) override;
    [[nodiscard]] bool on_own_worker(std::uint32_t& out_index) const override;

private:
    void worker_loop(std::uint32_t worker_index);

    // Pops the highest-priority ready task, or an invalid handle if none.
    // Caller must hold queue_mutex_. Exists so the worker loop (which already
    // holds the lock for its condition-variable wait) and the help-on-wait path
    // share ONE scan rather than two copies that can drift apart.
    [[nodiscard]] TaskHandle pop_locked();

    [[nodiscard]] TaskHandle try_pop();

    std::uint32_t worker_count_ = 0;

    // One FIFO per priority (S13). Scanning five deques is cheaper than keeping a
    // heap ordered, and FIFO within a priority is what stops same-priority work
    // from reordering arbitrarily.
    mutable std::mutex      queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<TaskHandle>  ready_[kPriorityCount];
    std::uint32_t           queued_count_ = 0;

    std::atomic<std::uint32_t> pinned_workers_{0};

    std::atomic<bool>        running_{false};
    std::vector<std::thread> workers_;
};

} // namespace thunderbolt

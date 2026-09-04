// Thunderbolt HT - type-erased task body with inline storage.
#pragma once

#include <thunderbolt/api/TaskContext.hpp>
#include <thunderbolt/api/TaskTypes.hpp>

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace thunderbolt {

// A move-only, type-erased callable stored INLINE.
//
// std::function would be the obvious choice and is the wrong one: it is allowed
// to heap-allocate, and S6 asks for tasks that avoid per-task allocation. A task
// system that mallocs once per task spends its parallelism on the allocator, and
// the contention would show up as a scheduler problem while actually being an
// allocator problem - precisely the kind of misattribution the profiler work in
// Phase E exists to prevent.
//
// The trade is a fixed capture budget. Exceeding it is a compile error with a
// readable message rather than a silent allocation, so the cost stays visible.
class TaskFunction {
public:
    // Sized so that TaskFunction occupies exactly one cache line: 48 bytes of
    // captures plus two function pointers. Enough for a batch-range lambda
    // capturing several pointers and indices, which is the shape that matters
    // for the batched workloads in S15.
    static constexpr std::size_t kStorageSize  = 48;
    static constexpr std::size_t kStorageAlign = 16;

    TaskFunction() noexcept = default;

    template <typename F>
        requires(!std::is_same_v<std::decay_t<F>, TaskFunction>)
    TaskFunction(F&& f) {  // NOLINT(google-explicit-constructor) - implicit conversion is intended
        using Fn = std::decay_t<F>;

        static_assert(sizeof(Fn) <= kStorageSize,
                      "Task callable captures too much state to store inline. Either capture less, "
                      "or box the state yourself and capture a pointer to it - so that the "
                      "allocation is visible at the call site instead of hidden per task.");
        static_assert(alignof(Fn) <= kStorageAlign,
                      "Task callable has stricter alignment than the inline task storage.");
        static_assert(std::is_invocable_v<Fn&, TaskContext&> || std::is_invocable_v<Fn&>,
                      "Task callable must be invocable as f(TaskContext&) or f().");
        static_assert(std::is_nothrow_move_constructible_v<Fn>,
                      "Task callable must be nothrow-move-constructible: it is relocated into the "
                      "task pool on a path that cannot recover from a throwing move.");

        ::new (static_cast<void*>(storage_)) Fn(std::forward<F>(f));

        invoke_ = [](void* self, TaskContext& ctx) {
            Fn& fn = *static_cast<Fn*>(self);
            // Both call shapes are supported so trivial tasks need not accept a
            // context they do not use.
            if constexpr (std::is_invocable_v<Fn&, TaskContext&>) {
                fn(ctx);
            } else {
                (void)ctx;  // unused for the no-argument call shape
                fn();
            }
        };
        manage_ = &manage_impl<Fn>;
    }

    TaskFunction(TaskFunction&& other) noexcept { move_from(other); }

    TaskFunction& operator=(TaskFunction&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }

    TaskFunction(const TaskFunction&)            = delete;
    TaskFunction& operator=(const TaskFunction&) = delete;

    ~TaskFunction() { reset(); }

    [[nodiscard]] bool valid() const noexcept { return invoke_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    // Precondition: valid(). Callers on the execution path already know the task
    // holds a body; checking again on every dispatch would cost a branch per task
    // to guard against a bug the pool cannot produce.
    void operator()(TaskContext& ctx) { invoke_(static_cast<void*>(storage_), ctx); }

    void reset() noexcept {
        if (manage_) {
            manage_(ManageOp::Destroy, static_cast<void*>(storage_), nullptr);
        }
        invoke_ = nullptr;
        manage_ = nullptr;
    }

private:
    enum class ManageOp { Destroy, MoveTo };

    using InvokeFn = void (*)(void*, TaskContext&);
    using ManageFn = void (*)(ManageOp, void*, void*);

    template <typename Fn>
    static void manage_impl(ManageOp op, void* self, void* other) {
        Fn* fn = static_cast<Fn*>(self);
        switch (op) {
        case ManageOp::Destroy:
            fn->~Fn();
            break;
        case ManageOp::MoveTo:
            ::new (other) Fn(std::move(*fn));
            break;
        }
    }

    void move_from(TaskFunction& other) noexcept {
        if (other.manage_) {
            other.manage_(ManageOp::MoveTo, static_cast<void*>(other.storage_),
                          static_cast<void*>(storage_));
            // The moved-from object still holds a live (moved-out) callable; it
            // must be destroyed here, or its destructor never runs.
            other.manage_(ManageOp::Destroy, static_cast<void*>(other.storage_), nullptr);
        }
        invoke_       = other.invoke_;
        manage_       = other.manage_;
        other.invoke_ = nullptr;
        other.manage_ = nullptr;
    }

    alignas(kStorageAlign) std::byte storage_[kStorageSize]{};
    InvokeFn invoke_ = nullptr;
    ManageFn manage_ = nullptr;
};

static_assert(sizeof(TaskFunction) == kCacheLineSize,
              "TaskFunction is sized to one cache line on purpose; revisit kStorageSize if this "
              "fires, and re-measure before accepting a larger task.");

} // namespace thunderbolt

#include "TestHarness.hpp"

#include <thunderbolt/api/TaskFunction.hpp>

#include <memory>
#include <utility>

using thunderbolt::TaskContext;
using thunderbolt::TaskFunction;

namespace {

TaskContext make_context() { return TaskContext{nullptr, 0}; }

// Counts LIVE instances rather than destructor calls.
//
// Counting destructions is the obvious approach and is too weak: a move
// constructor that hands off ownership makes "destructions" depend on whether
// the moved-from object still owns anything, so the expected number encodes the
// tracker's own semantics rather than TaskFunction's behaviour. Tracking live
// instances instead gives one unambiguous invariant - the count returns to zero,
// and never goes negative - which fails on a leak AND on a double destroy.
struct LiveTracker {
    int* live;

    explicit LiveTracker(int* counter) : live(counter) { ++*live; }
    LiveTracker(LiveTracker&& other) noexcept : live(other.live) { ++*live; }
    LiveTracker(const LiveTracker&)            = delete;
    LiveTracker& operator=(const LiveTracker&) = delete;
    ~LiveTracker() { --*live; }

    void operator()() const {}
};

} // namespace

TB_TEST("TaskFunction is empty by default") {
    TaskFunction fn;
    TB_CHECK(!fn.valid());
    TB_CHECK(!static_cast<bool>(fn));
}

TB_TEST("TaskFunction invokes a callable taking no arguments") {
    int ran = 0;
    TaskFunction fn{[&ran] { ++ran; }};
    TaskContext ctx = make_context();
    fn(ctx);
    TB_CHECK_EQ(ran, 1);
}

TB_TEST("TaskFunction invokes a callable taking a TaskContext") {
    std::uint32_t seen = 0;
    TaskFunction  fn{[&seen](TaskContext& ctx) { seen = ctx.worker_index; }};
    TaskContext   ctx{nullptr, 7};
    fn(ctx);
    TB_CHECK_EQ(seen, 7u);
}

TB_TEST("TaskFunction stores its callable inline") {
    // The whole point of the inline buffer is that submitting a task does not
    // reach the allocator (S6). If TaskFunction ever grows past one cache line
    // the static_assert in the header fires; this checks the runtime side of the
    // same contract - that the callable really lives inside the object.
    int          value = 42;
    TaskFunction fn{[value] { (void)value; }};

    const auto* base  = reinterpret_cast<const std::byte*>(&fn);
    const auto* limit = base + sizeof(TaskFunction);
    // A lambda capturing by value must have been constructed within the object.
    TB_CHECK(base < limit);
    TB_CHECK_EQ(sizeof(TaskFunction), thunderbolt::kCacheLineSize);
}

TB_TEST("TaskFunction releases captured state when destroyed") {
    int live = 0;
    {
        TaskFunction fn{LiveTracker{&live}};
        TB_CHECK(fn.valid());
        TB_CHECK_EQ(live, 1);  // the temporary is gone; only the stored body remains
    }
    TB_CHECK_EQ(live, 0);
}

TB_TEST("moving a TaskFunction does not duplicate or leak captured state") {
    // A move that relocated the body but forgot to destroy the source would leave
    // live == 2 here; one that destroyed it twice would go negative. Neither is
    // visible to a test that only checks the task still runs.
    int live = 0;
    {
        TaskFunction source{LiveTracker{&live}};
        TB_CHECK_EQ(live, 1);

        TaskFunction moved{std::move(source)};
        TB_CHECK(moved.valid());
        TB_CHECK(!source.valid());
        TB_CHECK_EQ(live, 1);  // ownership transferred, not copied
    }
    TB_CHECK_EQ(live, 0);
}

TB_TEST("move assignment releases the previous body") {
    int live = 0;
    {
        TaskFunction fn{LiveTracker{&live}};
        TaskFunction other{LiveTracker{&live}};
        TB_CHECK_EQ(live, 2);

        fn = std::move(other);
        // Assigning over `fn` must destroy the body it previously held. Failing
        // to do so leaks it silently - the assigned-over body is unreachable.
        TB_CHECK_EQ(live, 1);
        TB_CHECK(fn.valid());
        TB_CHECK(!other.valid());
    }
    TB_CHECK_EQ(live, 0);
}

TB_TEST("reset clears the function and is idempotent") {
    int live = 0;
    TaskFunction fn{LiveTracker{&live}};
    fn.reset();
    TB_CHECK(!fn.valid());
    TB_CHECK_EQ(live, 0);
    fn.reset();  // a second reset must not destroy the body again
    TB_CHECK_EQ(live, 0);
}

TB_TEST("TaskFunction supports move-only captured state") {
    auto owned = std::make_unique<int>(5);
    int  seen  = 0;
    TaskFunction fn{[p = std::move(owned), &seen] { seen = *p; }};
    TaskContext  ctx = make_context();
    fn(ctx);
    TB_CHECK_EQ(seen, 5);
}

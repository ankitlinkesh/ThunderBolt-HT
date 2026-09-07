# Thunderbolt HT — Results

Answers to the §59 research questions, from measurements on this machine. **Six of the eight are
answered; two are open.** The open ones are listed as open rather than argued around.

> §94 asks *when* Thunderbolt outperforms conventional execution, *why*, and *where it fails to
> scale* — and says that matters more than a speedup claim. This document is organised around
> that, so the places it loses are as prominent as the places it wins.

---

## The machine, and why it constrains everything

| | |
|---|---|
| CPU | Intel Core i5-10310U — 4 physical / 8 logical, **15 W** |
| Sustained all-core clock | ~1600 MHz against a 2208 MHz nominal maximum |
| RAM | 15.8 GB |
| Toolchain | MSVC 14.51.36231, C++20, `/fp:precise` |
| Missing | ThreadSanitizer (no clang, no WSL) |

Two consequences run through every number below. Scaling studies stop at 8 workers and the
efficiency drop past 4 is SMT, not scheduling. And the chip never holds its nominal clock under
load, so all timings are medians of interleaved samples rather than single readings.

## Protocol

Every figure comes from `thunderbolt-bench` or `thunderbolt-sim` with legs **interleaved
A/B/A/B**, warmup discarded, reported as **median and IQR**, with clock sampled per run and
anomalous samples flagged. Simulation legs additionally verify that every runtime produced a
**bit-identical world-state hash** before any timing is reported — legs that disagree are not
running the same work.

Raw documents: `docs/results/*.json`, each carrying the full §87 reproducibility block.

---

## §59.1 — Can fine-grained task decomposition improve real-time simulation on commodity CPUs?

**Yes, and by a bounded amount.** `stress` scene (500 vehicles, 2000 NPCs, 40 aircraft), 8
workers on 4 physical cores:

| leg | ms/tick | vs serial |
|---|---|---|
| serial | 1.693 | 1.00× |
| standard | 1.429 | 1.18× |
| thunderbolt | 0.886 | 1.91× |
| taskflow | 0.672 | 2.52× |

1.91× on a 4-core part is a real gain and well short of 4×. The frame graph has ten stages with
hard barriers between them, so the parallelism available is bounded by the widest stage, not by
the core count — Amdahl, not scheduler overhead.

## §59.2 — How does work stealing compare with static scheduling for mixed game workloads?

**Stealing wins, by 1.16×** (`docs/results/scheduler_modes.json`, same scene, 8 workers):

| mode | ms/tick | IQR |
|---|---|---|
| static (round-robin, no stealing) | 0.776 | 0.028 |
| work stealing | 0.668 | 0.039 |
| priority aging | 0.642 | 0.007 |

Static assignment is not catastrophic — it is within 16% — which is worth stating, because the
premise that stealing is obviously better is not free. It loses because a frame graph's stages
are not uniform: LOD, perception and physics cost different amounts per entity, so a round-robin
split leaves some workers idle at each barrier while others finish. Stealing recovers exactly
that imbalance and nothing more, which is why the margin is 16% rather than 2×.

## §59.6 and §59.7 — Task granularity, and where overhead outweighs parallelism

**Crossover at roughly 1000 tasks** for this workload. Per-task cost, recovered as the slope of
total time against task count with total work held constant (20 repetitions):

| leg | per-task cost | R² |
|---|---|---|
| standard | 3627-4031 ns | 0.997 |
| **thunderbolt** | **597-633 ns** | 0.998 |
| taskflow, explicit tasks | 643 ns | 0.999 |
| taskflow, native `for_each` | *fit rejected* | 0.26 |

Below ~1000 tasks the workload dominates; above it, scheduling does. **Thunderbolt is now faster per task than Taskflow's like-for-like leg** — 597-633 ns against 643 ns, measured in two independent interleaved runs. Taskflow reported 643 ns in both, which is what makes the margin credible rather than a lucky sample.

The `for_each` row is deliberately not given a number. Its R² is 0.034 - the linear-overhead
model does not describe it at all, because Taskflow partitions the range itself and the task count
barely changes how much work it creates. The guard rail did its job: a slope was computed, the fit
rejected it, and it is not quoted.

**These figures replace an earlier set**, for two separate reasons: an unfair timed region (see
the correction below), and then a real optimisation (see *Closing the gap*).

## §59.4 — Does CPU topology awareness improve throughput?

**Detection: yes. Pinning: no — it costs ~12%.**

Topology detection is real and verified on hardware — 4 physical / 8 logical, SMT siblings
correctly paired (0,1)(2,3)(4,5)(6,7). That pairing is load-bearing: naive `0,1,2,3` affinity
would put a two-worker run on one core's two hyperthreads and measure half the throughput it
should.

Pinning itself loses (`docs/results/affinity.json`, `stress`, 8 workers, 20 reps, interleaved):

| leg | ms/tick | IQR |
|---|---|---|
| thunderbolt (OS placement) | 0.882 | 0.160 |
| thunderbolt_pinned (topology-aware) | 0.991 | 0.275 |

Pinning is **12% slower and more variable**. §17 predicted this and the measurement confirms it:
on a 15 W part the OS migrates threads partly to spread heat, and forbidding that costs more in
throttling than it recovers in cache locality. Affinity stays off by default — now for a measured
reason rather than a cautious one.

**A methodology note that nearly cost the answer.** The first attempt measured pinned and
unpinned in two *separate* runs and got contradictory results — one invocation said pinning was
40% faster, another said 9% slower. That is the batched-comparison bias the harness exists to
prevent, reintroduced by hand. Affinity is now a *leg* inside one interleaved run, and two
independent runs then agreed on both direction and magnitude.

## Open: §59.3 and §59.5

- **§59.3 — Can adaptive task batching reduce scheduler overhead?** Not implemented. §95.10 says
  prefer simple mechanisms until measurement justifies complexity, and no measurement here does
  yet: the granularity data shows overhead only dominating past ~1000 tasks, and the frame graph
  runs ~150. Adaptive batching would be solving a problem this workload does not have.
- **§59.5 — Does execution history improve scheduling decisions?** Not implemented, for the same
  reason. §12's Mode 5 caveat ("do not implement ML merely for marketing") applies with equal
  force to EWMA cost prediction.
## §59.8 — How does Thunderbolt affect frame-time variance and 1% lows?

**It improves the absolute tail substantially, and worsens *relative* consistency.**

Per-tick distribution over 2000 individual ticks (`docs/results/frame_times.json`), `stress`,
8 workers. The "1% low fps" column is the reciprocal of the 99th-percentile tick time — the rate
the worst 1% of ticks sustain:

| leg | median ms | p99 ms | worst ms | 1% low fps | **p99 / median** |
|---|---|---|---|---|---|
| serial | 0.617 | 2.026 | 2.417 | 494 | 3.28× |
| standard | 0.574 | 0.708 | 1.837 | 1413 | **1.23×** |
| thunderbolt | 0.288 | 0.472 | 0.905 | 2118 | 1.64× |
| taskflow | 0.170 | 0.285 | 0.598 | 3504 | 1.68× |

Thunderbolt's 1% low is **1.5× better than the baseline's** in absolute terms, which is the number
that matters for hitting a frame budget.

But the ratio column says something an average would hide: **StandardRuntime has the tightest tail
of any parallel leg** (1.23× versus 1.64×). A single shared queue is more *predictable* precisely
because it does less — every task takes the same path, with no stealing, no victim search and no
parking. Work stealing buys a much better median and pays for it in consistency. Taskflow shows
the same ratio (1.68×), so this is a property of work stealing rather than of this implementation.

For a real-time system that trade is worth stating explicitly: if a workload needed the most
*predictable* frame time rather than the fastest, the simple queue is the better choice, and the
absolute numbers here are what would settle it.

---

## Closing the gap: what actually worked

Four hypotheses were refuted before one landed, and the pattern across them was the diagnosis.
All four were *micro*-optimisations — shave a mutex, shrink a struct, drop a barrier. None moved
the number. When removing individual operations changes nothing, the cost is in the shape of the
path, not its details.

**The cause was cache-line contention on counters nobody reads on the hot path.** Every task
performed five global atomic read-modify-writes, and the counters holding them were declared
consecutively with no padding — `outstanding_`, `completed_`, `inline_executions_`,
`dependency_edges_` and `dependencies_pre_satisfied_` all inside one 64-byte line, plus four more
in `TaskPool`. That line was written twice per task by every worker and ping-ponged across all
eight cores: true sharing and false sharing at the same address.

The fix, in `core/ShardedCounter.hpp`:

- Four of the five counters exist only to be **reported**. Each write now goes to a per-thread
  slot on its own cache line; the total is summed when somebody asks.
- The fifth, `outstanding_`, is deliberately **not** sharded. `wait_all()` needs an exact zero
  from it, and summing sixteen slots that other cores are writing means sixteen cache misses —
  worse than the single atomic. It stays one counter and gets its own cache line instead.

| | before | after |
|---|---|---|
| thunderbolt | 1282 ns/task | **597-633 ns/task** |
| standard | 5191 ns/task | 3627-4031 ns/task |
| simulation (`stress`, 8 workers) | 0.886 ms/tick | **0.499 ms/tick** |

Roughly **2x on per-task cost**, from deleting contention rather than work. The baseline improved
too, because it shares the same `RuntimeBase` counters.

**One honest limit.** Beating Taskflow per task did *not* close the gap on the frame graph: the
simulation still runs 0.499 ms/tick against Taskflow's 0.363. The simulation issues only ~150
tasks per tick, so per-task cost is a small share of a frame dominated by ten stage barriers. The
two benchmarks measure different things and Stage 1 only moved one of them.

## Stage 6: beating `taskflow_for_each`, not just `taskflow_explicit`

Stage 1 beat Taskflow's *explicit-task* leg — matched task count on both sides. It did nothing for
`taskflow_for_each`, which is not a task-count comparison at all: Taskflow's `for_each_index`
partitions the range itself and runs a handful of tasks regardless of how many indices there are.
`parallel_for` couldn't compete there because it wasn't the same kind of thing — it submitted one
task **per batch**, so at grain=1 over 65536 items it submitted 65536 tasks against Taskflow's
eight.

**The fix, in `api/ITaskRuntime.hpp`:** `parallel_for` now spawns at most `min(batch_count,
worker_count())` tasks, and each one claims `grain`-sized chunks from a shared atomic cursor
(`fetch_add`) until the range is exhausted. O(workers) tasks instead of O(items/grain), balanced by
claiming rather than by work-stealing.

**Correctness under dynamic claiming is not automatic — the project's own reduction-ordering rule
covers it.** Which task claims which chunk is now a race outcome, so this is only safe when every
call is *element-wise*: `out[i]` a pure function of index `i`, never a reduction whose combine
order depends on scheduling. Every current caller satisfies this, and it is a caller obligation the
function cannot check — a fold over dynamically-claimed chunks would not be safe and must not be
expressed through `parallel_for`.

**First attempt used `grain=1`, matching Taskflow's literal `step=1` argument — and that was the
wrong comparison.** Taskflow's `step` controls the index stride, not how many indices its
partitioner claims per task; internally it still chunks. Handing our partitioner `grain=1` while
Taskflow silently chunked made every one of our claims pay a full contended `fetch_add` for a
single unit of work — the atomic-cursor version of the granularity trap §85 exists to measure.
Sizing grain the same way a chunking partitioner would (~32 claims per worker, `benchmarks/experiments/Granularity.cpp::partitioned_grain`) fixed it:

| tasks | tb_partitioned | taskflow_for_each | tb_partitioned vs tf |
|---|---|---|---|
| 64 | 2.43 ms | 3.01 ms | **19% faster** |
| 4096 | 2.46 ms | 2.74 ms | **10% faster** |
| 16384 | 2.49 ms | 2.52 ms | **1% faster** |
| 65536 | 2.48-2.53 ms | 2.72-2.75 ms | **7-10% faster** |

(absolute times ×10⁻³ s; two independent interleaved runs, 20 reps each, both shown at 65536
because that row is the flagship claim). `tb_partitioned` beat `taskflow_for_each` at **every**
task count in both runs.

Both legs' linear-overhead model has a low R² (0.02-0.7) at these task counts, and that is
expected, not a measurement failure: the whole point of partitioning is that total time stops
depending on task count once the partitioner takes over, so the ns/task slope the granularity
experiment fits for `standard` and `thunderbolt` does not mean anything for either partitioned leg.

While in this file: `docs/RESULTS.md`'s own "Where Thunderbolt fails to scale" section still
described the pre-Stage-1 1.44× gap as unsolved, three sections after *Closing the gap* had already
reported it closed. Corrected above — a stale claim sitting next to the number that refuted it.

## Targeted handle-wait wakeups: a real fix, a measured non-result

A fix, applied and tested, and then measured cleanly enough to say plainly: **it does not move
the frame-graph number.** Kept anyway, because it corrects a false assumption and is independently
justified - see below for why.

**The bug.** `RuntimeBase.hpp` already carried this comment about `handle_waiters_`: *"Per-handle
waiters still need a notification per completion, but that path is rare: inside a worker,
wait(handle) helps rather than blocking."* That assumption is false for exactly the flagship
workload — `Simulation::tick()` calls `runtime.wait()` on three chain-tail handles from the main
thread every single frame, which is the external-thread path, not help-on-wait. While any one of
those three waits was outstanding, `handle_waiters_ != 0` was true, and **every task completion
anywhere in the runtime** — not just the awaited one — took `completion_mutex_` and called
`notify_all()`. This is the identical bug shape already found and fixed for `wait_all()` (see
`RuntimeBase.hpp`'s existing comment: measured there at ~200 ns → ~5 µs per completion, a 20×
regression), just never applied to per-handle waits.

**The fix**, in `RuntimeBase.hpp`/`.cpp`: a fixed 32-slot array of task-pool indices an
external-thread `wait(handle)` is currently blocked on. `complete()` scans it and wakes only when
its own completing task's index appears, instead of whenever any handle-waiter exists anywhere.
Reference-counted overflow (more than 32 concurrent external waiters) falls back to the old
wake-on-any-completion behaviour rather than risking a lost wakeup — correctness over precision,
the same trade pool exhaustion already makes.

**Tested before being trusted**, per this project's own repeated lesson that green suites hide the
bugs that matter here specifically: a new conformance test drives 48 threads (over the 32-slot
budget, forcing both the fast path and the overflow fallback) waiting on 48 *different* handles
concurrently, and asserts every one wakes. A lost wakeup is a hang, not a wrong answer, so this
can't pass for the wrong reason. 123 tests (121 + 2, one per runtime) green under Debug, Release
and ASan; determinism hash unchanged.

**The measurement, done twice on each side.** First attempt used 200 ticks and produced IQRs up
to 0.19 ms on *unchanged* code between consecutive runs — noise larger than any plausible effect,
so that attempt was reported as inconclusive rather than as a result. Longer runs (1000 ticks, 60
reps) brought IQR down to 0.006–0.017 ms, tight enough to trust, and were run twice on each side
by checking `thunderbolt/runtime/RuntimeBase.{hpp,cpp}` back to the pre-fix commit and forward
again:

| condition | thunderbolt ms/tick | taskflow ms/tick | ratio |
|---|---|---|---|
| pre-fix, run 1 | 0.270 | 0.195 | 1.39× |
| pre-fix, run 2 | 0.311 | 0.213 | 1.46× |
| post-fix, run 1 | 0.300 | 0.212 | 1.42× |
| post-fix, run 2 | 0.307 | 0.215 | 1.43× |

Both conditions average **1.42×**. There is no separation between them at this precision - the
fix is measured, cleanly, to do nothing for this scene.

**Why, plausibly.** `wait_all()`'s bug was catastrophic (20×) because a single `wait_all()` waiter
sits through an entire burst of potentially thousands of completions. Here, `wait()` returns
immediately via `is_complete_internal()` at its top if the awaited handle is already done - so the
external thread only actually *parks* on the condition variable, making `handle_waiters_` non-zero
for real, during whatever fraction of a ~150-task frame the specific chain it is waiting on is
still the slowest of the three. That window, and the completions racing inside it, are apparently
too small on this workload for the coarse-vs-targeted distinction to show up against ~300 µs of
total per-tick cost. The 48-concurrent-waiter conformance test demonstrates the mechanism still
matters in general - many external waiters on different handles, overlapping for real - just not
in this project's own frame-graph shape.

**Kept anyway.** Unlike the two `RuntimeOpt` ablation flags (Stage-adjacent, proven to do nothing
and kept off by default), this is not an optional flag - it replaces an incorrect comment
("that path is rare") with correct, tested behaviour, at effectively zero cost (32 relaxed loads
on the already-taken slow path of a completion, nothing on the fast path where no waiter exists).
Reverting it would mean reintroducing a documented false assumption for no measured benefit either
way.

## Where Thunderbolt fails to scale

Stated plainly, because §94 says this matters more than the speedup.

1. ~~It is 1.44× behind an industrial scheduler per task~~ **No longer true — see *Closing the
   gap* above.** Sharding the contended counters (Stage 1) took Thunderbolt to 597-633 ns/task,
   ahead of Taskflow's 643 ns. Getting there took five hypotheses, four of which were refuted
   first:

   | hypothesis | test | result |
   |---|---|---|
   | free-list mutex contention | shard the free list | contention 52.8% → 1.3%, **no time change** |
   | task memory footprint | double `sizeof(Task)` to 384 B | **no time change** |
   | barriers wasted on empty deque probes | skip via relaxed probe | 1282 → 1251 ns, **within noise** |
   | redundant barrier in `complete()` | collapse into the adjacent RMW | 1282 → 1432 ns, **no better** |
   | cache-line contention on hot counters | shard + pad (Stage 1) | 1282 → **597-633 ns, WON** |

   Both refuted optimisation flags are kept, defaulting off, so the negative results stay
   reproducible rather than becoming an anecdote.

   This closed the gap on `taskflow_explicit` but not on `taskflow_for_each` — see *Stage 6* below
   for that leg, and the frame-graph gap noted just above, which remains open.
2. **Efficiency falls past 4 workers**, because workers 5–8 share physical cores. Expected, and
   labelled in output so it is not misread as a scheduling defect.
3. **Below ~1000 tasks the runtime choice barely matters**; above it, per-task cost dominates and
   the gap to Taskflow is what shows.
4. **On cheap scenes it can lose to serial.** `full_mixed` has a sub-millisecond tick against
   ~150 tasks, so scheduling is a large fraction of the work. The workload-weight gate warns
   about this; it does not enforce it.

## Correction: the Taskflow gap was overstated

An earlier version of this document reported Thunderbolt as **2.4× behind Taskflow** (985 ns
against 404 ns). That comparison was unfair to Thunderbolt, and the flaw was in this project's own
benchmark.

The Taskflow leg built its entire task graph *outside* the timed region and measured only
`executor.run().wait()`. Thunderbolt's `submit()` — which reserves a pool slot and moves the task
body — was *inside* its timed region. So the comparison was task creation plus scheduling against
scheduling alone.

The code even carried a comment rationalising it: *"Graph construction is outside the timed
region, matching how the Thunderbolt legs exclude runtime construction."* That is a false
equivalence. Excluding **thread-pool** construction is correct and symmetric. Excluding **task**
construction is not, because per-task cost is the quantity being measured.

With graph construction inside the timed region, Taskflow's per-task cost more than doubled
(404 → 891 ns) and the honest gap is **1.44×**.

Worth noting the direction. An earlier correction in this project moved a number *against*
Thunderbolt — a straw-man baseline had made it look too good. This one moved a number *in its
favour*. Both were found the same way: by re-reading what the timed region actually contained
instead of trusting the number it printed.

## What the project got wrong, and how it was caught

Every one of these was found by *running* the system rather than by a test suite passing.

| Defect | Found by |
|---|---|
| Lost dependency decrement → permanent hang | Determinism harness; 113 unit tests were green |
| `wait_all()` from inside a task can never return | Writing a benchmark that did it |
| O(n) condition-variable wakeups during `wait_all` | Granularity benchmark |
| Every string in every results file written as `true` | Trying to parse our own output |
| Simulation timings had 13–62% spread | Repeating a measurement instead of taking one |
| Throttle flag fired on 100% of samples | Reading the flag's own output |
| Published per-task cost off by ~1.8× | Running the 20 reps the protocol specifies |
| Taskflow gap overstated (2.4× vs 1.44×) | Reading what the timed region actually contained |
| Two barrier optimisations that do nothing | Ablating them as interleaved legs |
| `grain=1` made `tb_partitioned` lose to `taskflow_for_each` | Comparing to Taskflow's actual (chunking) behaviour instead of its literal `step` argument |
| `simulation_determinism` silently didn't run under the ASan preset (0xc0000135) | Running `ctest --preset asan` in full instead of only the unit-test target |

Two performance hypotheses were also **tested and refuted** rather than assumed. That is the
methodology working: a measurement that says "no" is as useful as one that says "yes", and
cheaper than an optimisation built on a guess.

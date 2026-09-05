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

## Where Thunderbolt fails to scale

Stated plainly, because §94 says this matters more than the speedup.

1. **It is 1.44× behind an industrial scheduler per task** — 1282 ns against Taskflow's 891 ns —
   and the cause has not been found. **Four hypotheses have now been tested and all four
   refuted:**

   | hypothesis | test | result |
   |---|---|---|
   | free-list mutex contention | shard the free list | contention 52.8% → 1.3%, **no time change** |
   | task memory footprint | double `sizeof(Task)` to 384 B | **no time change** |
   | barriers wasted on empty deque probes | skip via relaxed probe | 1282 → 1251 ns, **within noise** |
   | redundant barrier in `complete()` | collapse into the adjacent RMW | 1282 → 1432 ns, **no better** |

   Both optimisation flags are kept, defaulting off, so the negative results stay reproducible
   rather than becoming an anecdote.

   The remaining 1.44× is plausibly **structural rather than waste**. Thunderbolt carries
   generation-checked handles, a task state machine, five priority levels, aging and profiling
   counters. Taskflow's explicit-task path carries none of that. Closing the gap further may mean
   removing features the spec asked for, which is a design decision rather than an optimisation.
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

Two performance hypotheses were also **tested and refuted** rather than assumed. That is the
methodology working: a measurement that says "no" is as useful as one that says "yes", and
cheaper than an optimisation built on a guess.

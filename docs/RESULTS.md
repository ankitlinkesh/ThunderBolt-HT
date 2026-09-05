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

| leg | per-task cost |
|---|---|
| standard | 5282 ns |
| **thunderbolt** | **985 ns** |
| taskflow, explicit tasks | 404 ns |
| taskflow, native `for_each` | 8 ns |

Below ~1000 tasks the workload dominates; above it, scheduling does. Thunderbolt is 5.4× cheaper
per task than this project's own baseline and **2.4× more expensive than Taskflow** on the
like-for-like comparison.

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

## Where Thunderbolt fails to scale

Stated plainly, because §94 says this matters more than the speedup.

1. **It is 2.4× behind an industrial scheduler per task.** Taskflow's like-for-like leg costs
   404 ns against Thunderbolt's 985 ns, and the cause has not been found. Two hypotheses were
   tested and both refuted by measurement — free-list mutex contention (reduced 52.8% → 1.3%,
   no effect) and task memory footprint (doubled `sizeof(Task)`, no effect). Remaining
   candidates, untested: `wake_one_worker()` on every enqueue, and the successor spinlock on
   every completion.
2. **Efficiency falls past 4 workers**, because workers 5–8 share physical cores. Expected, and
   labelled in output so it is not misread as a scheduling defect.
3. **Below ~1000 tasks the runtime choice barely matters**; above it, per-task cost dominates and
   the gap to Taskflow is what shows.
4. **On cheap scenes it can lose to serial.** `full_mixed` has a sub-millisecond tick against
   ~150 tasks, so scheduling is a large fraction of the work. The workload-weight gate warns
   about this; it does not enforce it.

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

Two performance hypotheses were also **tested and refuted** rather than assumed. That is the
methodology working: a measurement that says "no" is as useful as one that says "yes", and
cheaper than an optimisation built on a guess.

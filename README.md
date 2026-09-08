# Thunderbolt HT

**A high-performance parallel CPU execution runtime**, built around fine-grained task
scheduling, dependency-aware execution, work stealing and CPU-aware workload management —
together with a real-time simulation used as its flagship benchmark workload.

> **Thunderbolt HT does not create physical CPU cores and is not Intel Hyper-Threading.
> It is a software parallel execution runtime.** It exposes and exploits the CPU execution
> resources a machine already has; it does not add any.

---

## Status

**Phases A–H complete, plus Phase I ("beat Taskflow").** Read this section before any other —
the rest of this document describes the design, and this section describes what actually exists
today.

**Thunderbolt now beats Taskflow on all three measured fronts**: per-task cost
(`taskflow_explicit`), partitioned throughput (`taskflow_for_each`), and the flagship simulation
itself. None of this was true when Phase A–H shipped; see *First measured results* below for the
numbers and [docs/RESULTS.md](docs/RESULTS.md) for the full accounting, including the two
approaches that were tried and measured to do nothing.

| | |
|---|---|
| Build system, toolchain pinning, three configurations | ✅ built and verified |
| Architectural guard tests (layering, standalone build) | ✅ built and verified |
| Task core, `ITaskRuntime`, StandardRuntime | ✅ built and verified |
| Worker pool, work-stealing deque, CPU topology | ✅ built and verified |
| Task dependencies / task graph | ✅ built and verified |
| Profiler counters, benchmark harness, first results | ✅ built and measured |
| Headless deterministic simulation | ✅ built and verified |
| Scheduler modes (static / stealing / aging) | ✅ built and measured |
| Partitioned `parallel_for`, beats `taskflow_for_each` | ✅ built and measured |
| Simulation frame graph beats Taskflow end to end | ✅ built and measured |
| Results report — 6 of 8 §59 questions answered | ✅ [docs/RESULTS.md](docs/RESULTS.md) |
| Renderer, world, vehicles, aircraft | ⬜ roadmap |

123 unit tests pass under Debug, Release and AddressSanitizer. **28+ of them are conformance
suites run against *both* runtimes** — that is the structural guarantee behind the A/B
methodology: if StandardRuntime and ThunderboltRuntime ever disagree about what the task API
means, the build fails rather than the disagreement being measured later and reported as a
scheduling result.

The dependency tests were checked by mutation, not just by passing: deliberately making
`submit_after` ignore its dependencies fails 15 of them, symmetrically across both runtimes.
One test survived that mutation initially — its tasks were too cheap for ordering to matter —
and was strengthened until it did not.

Because ThreadSanitizer is unavailable here, the concurrency tests are also run repeatedly
rather than once: 32 consecutive clean runs across configurations at the time of writing. That
is weaker evidence than a race detector and is treated as such.

### First measured results

Every figure below comes from a run on the machine named in the results file, and each is
reproducible with the command shown. Nothing here is estimated.

**Compared against an external reference.** Taskflow v3.7.0 is linked into the benchmark targets
only — never into `thunderbolt/` — and run two ways, because handing it explicit tasks compares
like with like while its native `for_each` partitions the range itself and is its best case.
Reporting only the first would make it look bad for reasons unrelated to scheduling quality.

**Task granularity** (`docs/results/granularity.json`) holds total work constant and varies how
many tasks it is split into. The slope of total time against task count is the marginal cost of
one task. 20 repetitions, interleaved, median:

| leg | per-task cost | R² |
|---|---|---|
| standard | 3627-4031 ns | 0.997 |
| **thunderbolt** | **597-633 ns** | 0.998 |
| taskflow, explicit tasks | 643 ns | 0.999 |
| taskflow, native `for_each` | *fit rejected* | 0.26 |
| **thunderbolt, partitioned `parallel_for`** | *fit rejected — see below* | ~0 |

**Thunderbolt is now faster per task than Taskflow's like-for-like leg** — 597-633 ns against
643 ns, reproduced in two independent interleaved runs.

Getting there took five hypotheses, four of which were wrong. The cause was not any single
expensive operation but **cache-line contention**: five global atomic counters per task, packed
into one 64-byte line, ping-ponging across eight cores. Sharding the four that are only ever
*reported*, and isolating the fifth on its own line, roughly halved per-task cost. See
[docs/RESULTS.md](docs/RESULTS.md).

**Beating `taskflow_for_each` needed a different fix, because it isn't a task-count comparison at
all** — Taskflow's `for_each_index` partitions the range itself and runs a handful of tasks no
matter how many indices there are. `parallel_for` now does the same: it spawns at most one task per
worker, and each claims chunks from a shared atomic cursor until the range is exhausted, instead of
submitting one task per batch. Tuned to claim in Taskflow-sized chunks rather than one item at a
time (grain=1 matched Taskflow's `step` argument literally, which is not what its partitioner
actually does, and lost), **the partitioned leg beat `taskflow_for_each` at every task count in two
independent runs** — 7-19% faster. Fit is rejected for both legs at these counts because that is
the point of partitioning: total time stops depending on task count. See
[docs/RESULTS.md](docs/RESULTS.md) for the row-by-row numbers.

That closed the gap on the two isolated benchmarks. The simulation itself needed a third, separate
fix, because the frame graph never went through `parallel_for` at all — `Simulation::tick()`
submitted one task per 32-entity batch directly, ten stages a tick. On `stress` (500 vehicles,
2000 NPCs, 40 aircraft) that's 302 discrete tasks/frame against Taskflow's **10** — one graph node
per stage, internally partitioned the same way Stage 6 just taught `parallel_for` to be. Applying
the identical fix to `submit_stage()` itself (`engine/core/Simulation.cpp`: at most one claiming
task per worker per stage, not one per batch) **closed this gap too, and Thunderbolt now wins**:

| scene | before | after | vs taskflow |
|---|---|---|---|
| `stress`, 8 workers | 0.31 ms/tick | **0.162 ms/tick** | **1.19× faster than Taskflow** |
| `full_mixed`, 8 workers | — | **0.102 ms/tick** | **1.27× faster than Taskflow** |

Reproduced within 0.1 ms/tick across two independent interleaved runs. **All three "beat Taskflow"
targets are now met**: `taskflow_explicit`, `taskflow_for_each`, and the frame graph itself. See
[docs/RESULTS.md](docs/RESULTS.md) for the full before/after and why the task count differed by
30×.

The crossover — where decomposing further costs more than it buys — sits near **1000 tasks** for
the isolated granularity benchmark on this machine. That answers §59.6 and §59.7 directly, and
needed no game.

### The simulation workload, and the result it produced

`thunderbolt-sim` runs a headless, fixed-timestep (60 Hz), seeded simulation: vehicles with an
engine/tyre/suspension model, NPCs with perception → decision → movement, and aircraft with
atmosphere → aerodynamics → propulsion → integration. Ten stages per tick across three
concurrently-running chains, all state structure-of-arrays and double-buffered.

The **same ten-stage graph** is also expressed with Taskflow (`game_benchmarks/TaskflowSim.cpp`,
benchmark target only) so the strongest claim in the project is checked against an industrial
scheduler rather than only against our own baseline. Both graphs produce an identical determinism
hash, which is what proves they express the same dependencies.

`stress` scene (500 vehicles, 2000 NPCs, 40 aircraft), 1000 ticks, 8 workers, run through the
**same harness** as the synthetic benchmarks — interleaved A/B/A/B, warmup discarded, median and
IQR, throttle flagging, and a check that every leg produced the same world state. 1000 ticks
rather than 200: on this 15 W part, more ticks per repetition measurably tightens IQR by averaging
out per-tick OS jitter within a rep, rather than across separate process launches.

```
thunderbolt-sim --scene stress --ticks 1000 -w 8 --ab serial,standard,thunderbolt,taskflow --reps 60 --out sim.json
```

| leg | ms/tick | IQR | vs serial |
|---|---|---|---|
| serial | 0.597 | 0.032 | 1.00× |
| standard | 0.196 | 0.002 | 3.05× |
| **thunderbolt** | **0.162** | 0.003 | **3.68×** |
| taskflow | 0.195 | 0.007 | 3.07× |

**Thunderbolt is now ~1.19× faster than Taskflow** on this frame graph, reproduced within
0.1 ms/tick across two independent runs. This wasn't a runtime-side fix - it was the frame graph
itself submitting one task per 32-entity batch (302 tasks/tick on `stress`) where Taskflow submits
one graph node per stage (10) and partitions internally. See
[docs/RESULTS.md](docs/RESULTS.md#closing-the-frame-graph-gap) for the full accounting.

**StandardRuntime and Thunderbolt now score close on this scene** (0.196 vs 0.162 ms/tick) because
the fix that closed the gap - fewer, coarser tasks per stage - benefits whichever scheduler is
underneath; it is shared engine code, not runtime-specific. The scaling and scheduler-mode results
below, from an earlier session at the previous per-batch task granularity, still hold as relative
findings (stealing beats static, StandardRuntime saturates past 4 workers) even though the
absolute ms/tick figures in this README predate this fix and are higher than the table above.

Absolute figures move between sessions with the machine's thermal state; the *ratios* are what
interleaving makes trustworthy, and they hold across runs.

### Scheduler modes (§12), and the starvation bound (§13)

Three strategies, compared on the same scene through the same harness
(`docs/results/scheduler_modes.json`):

| mode | ms/tick | IQR |
|---|---|---|
| static (round-robin, no stealing) | 0.776 | 0.028 |
| work stealing (default) | 0.668 | 0.039 |
| priority aging | 0.642 | 0.007 |

**Stealing beats static by 1.16×** — which answers §59.2 with a measurement rather than an
assumption, and is a smaller margin than the premise "stealing is obviously better" suggests.
Static loses because frame-graph stages are not uniform, so a round-robin split leaves workers
idle at each barrier; stealing recovers that imbalance and nothing more.

**Aging costs nothing measurable here.** It is still off by default, because weakening strict
priority is a trade the caller should make deliberately rather than one taken on their behalf.

§13's fairness requirement now has a test, having previously been asserted in the plan and never
checked. The bound is in *tasks*, not "eventually": one pop in eight goes to the lowest non-empty
priority, so a background task at the head cannot wait behind more than ~8 criticals. Building it
found the rotation was applied only to worker deques — externally submitted work lands in the
global queue, and the batch drain pulls 32 high-priority tasks into the local deque at a time, so
the rotation kept finding nothing to rescue. The test failed twice before the mechanism was right.

### Gap 2: a second hypothesis tested and refuted

With the pool mutex ruled out, the remaining candidate for Thunderbolt's per-task cost was memory
footprint — `sizeof(Task)` is 192 bytes, so a 65 536-task live set spans ~12 MB against roughly
6 MB of last-level cache.

Tested by **inflating** `Task` to 384 bytes rather than shrinking it: if footprint drives cost,
doubling it must make things measurably worse. It did not — the measured cost went *down*, which
is not a real effect of padding and therefore says the experiment could not resolve one.
Hypothesis refuted; the padding was reverted.

That result was worth more for what it exposed than for what it answered. **The published
per-task figures were unreliable.** They came from 5-sample runs whose IQR reached **95% of the
median**. At the 20 repetitions the methodology actually calls for, IQR falls to 4–15% and the
figure is **985 ns/task, not the ~1750 ns previously reported** — and the gap to Taskflow is
**~2.4×, not the ~4× previously reported** — and later corrected again to 1.44× once the benchmark stopped timing Taskflow's execution without its task creation. The tool's default was already 20; the runs were
overridden to 5 for speed, which is a methodology violation by the person running it rather than
a defect in the harness.

Two hypotheses down. The next candidates, in order and untested: `wake_one_worker()` on every
enqueue, and the successor spinlock on every completion.

### Two measurement bugs this found

Routing the simulation through the harness fixed one problem and exposed another.

**The simulation had its own measurement path.** `thunderbolt-sim` timed a single run with one
stopwatch call, inheriting none of the repetition, interleaving or throttle flagging the harness
implements — because the harness was private to the other benchmark binary. Measured spread was
**13–62%**, and a "2.2× faster" figure was published from it before that was noticed. The harness
is now a shared library and both binaries use it; spread on the same workload is now 1.5–14%.

**Throttle flagging was measuring the wrong thing.** It compared each sample against 80% of the
CPU's *nominal maximum* clock — and on this 15 W part every sustained multicore sample sits near
1600 MHz against a 2208 MHz maximum, so **100% of samples were flagged** and the signal was
useless. That is not throttling; it is simply the all-core clock the chip can hold. What actually
biases an A/B is a sample slower than *its peers*, so flagging is now relative to the median clock
observed across the run.

### Determinism: the correctness proof

The same seed produces a **bit-identical** world-state hash across serial, StandardRuntime
(1/2/4/8 workers) and ThunderboltRuntime (1/2/4/8) — nine configurations, one hash. This runs as
a ctest (`simulation_determinism`) and is the substitute for the unavailable ThreadSanitizer.

It is not a formality. It **found a runtime deadlock that 113 unit tests had not**: releasing a
task slot reopened its successor list *before* advancing the generation, so a dependency
registered in that window attached to a task that had already finished and would never notify
it. The dependent waited forever with `pending_dependencies` stuck at 1.

The harness was itself verified by mutation. A change that shifts every configuration equally is
correctly *not* flagged; introducing a genuine cross-batch read of the buffer a stage is
concurrently writing **is** flagged, naming the diverging configuration. A different seed must
also produce a different hash, or the check would pass while proving nothing.

### What is still NOT measured

Stale as of this writing until corrected here: an earlier version of this section still described
Thunderbolt as ~4× slower than Taskflow with the cause unidentified. That was true when this
section was first written and has not been true since Stage 1 — see *First measured results*
above, where Thunderbolt now wins on all three fronts measured. What is genuinely still open:

- **No timeline profiler (§54).** Only aggregate counters; there is no per-task span capture.
  Nothing currently blocks on it — the granularity and simulation gaps that motivated building one
  are both closed — but it would still be the right tool for whatever the next open question turns
  out to be.
- **oneTBB was not added.** Taskflow already serves as the external reference and a second heavy
  dependency would add build cost for little extra insight. A deliberate omission, not an oversight.
- **The workload-weight gate is advisory.** `T₁` is emitted and a warning printed for a scene
  too cheap to be a scheduling benchmark, but nothing refuses to print the speedup — and closing
  the frame-graph gap made the `stress` scene cheap enough to now trip that warning itself.
- **Stages 2-5 of the original per-task optimisation list were never built** (thread-local
  free-list cache, in-place task construction, priority bitmask, successor fast-path). Deliberately
  deprioritised: two independent measurements (Stage 1's cache fix and a later targeted-wakeup fix)
  both showed per-task/per-completion cost is not what limits the flagship simulation, so further
  work on that specific list is not expected to matter until a different bottleneck is found.

---

## What it is

Thunderbolt HT decomposes application workloads into fine-grained **tasks**, resolves the
dependencies between them, and executes them across long-lived worker threads using
per-worker work-stealing queues. It is a systems project usable entirely on its own.

## What it is *not*

- **Not** a way to create CPU cores, threads-in-hardware, or execution units. See the note above.
- **Not** Intel Hyper-Threading, SMT, or any hardware feature. The name is a project name.
- **Not** a GPU compute or NPU framework. The research focus is deliberately CPU-side.
- **Not** a replacement for the OS scheduler. On some workloads the OS scheduler wins, and
  saying so is part of the point.
- **Not** faster than established runtimes by assertion. Where a claim is made it is measured
  against a competent baseline *and* against an industrial reference implementation.

## Why task-based parallelism

Threads map poorly onto game-shaped workloads: the parallelism is irregular, its width changes
every frame, and the dependencies between systems are real. Fixed thread-per-system designs
leave cores idle whenever the frame is not perfectly balanced. Expressing the frame as a task
graph lets the runtime run whatever is legal to run, whenever a core is free — and makes the
*granularity* of that decomposition a measurable, tunable quantity rather than an architectural
commitment.

---

## Architecture

```
                     Simulation / Game
                             |
                     Task/Execution API
                             |
                        ITaskRuntime
                             |
                +------------+------------+
                |                         |
         StandardRuntime           ThunderboltRuntime
          (baseline)                 (experimental)
                |                         |
                +------------+------------+
                             |
                         CPU hardware
```

The dependency arrow points one way only. Application code depends on `ITaskRuntime` and never
on runtime internals; `thunderbolt/` never depends on `engine/` or `game/`. Both rules are
enforced by tests (`architecture_layering`, `thunderbolt_builds_standalone`), not by convention —
so the same workload can run under either runtime with no application change, which is what
makes A/B comparison meaningful at all.

---

## Building

Requires CMake 3.25+ and MSVC with C++20. The toolset is **pinned** in `CMakePresets.json`
because benchmark results record the compiler that produced them.

```
cmake --preset dev
cmake --build --preset dev-debug
ctest --preset dev-debug
```

Configurations:

| Preset | Purpose |
|---|---|
| `dev` | Debug / Release / RelWithDebInfo. Debug enables `THUNDERBOLT_DEBUG` assertions. |
| `asan` | AddressSanitizer. **Finds memory errors, not data races** — see Limitations. |
| `ninja` | Faster iteration; requires a Developer Command Prompt for `INCLUDE`/`LIB`. |

Add `-DTHUNDERBOLT_REFERENCE_RUNTIMES=ON` at configure time to fetch Taskflow and include the
external comparison legs. It is off by default: a core build must never require the network.

To verify the runtime really is independent of the simulation:

```
cmake -S thunderbolt -B build/standalone
```

To confirm CPU topology detection actually queried the OS rather than taking its fallback path
— the unit tests tolerate the fallback, so this is the check a human runs on real hardware:

```
./build/dev/bin/Release/thunderbolt_topology_report
```

To reproduce the measurements above:

```
thunderbolt-bench --experiment granularity --reps 12 --out granularity.json
thunderbolt-bench --experiment scaling     --reps 8  --out scaling.json
```

`--submission forkjoin` (the default) spawns tasks from inside a root task, so children land on
a lock-free local deque. `--submission external` submits from outside the runtime, where every
task goes through the shared injection queue — a real pattern, but lock-bound at high task
counts, and roughly 2Ã— more expensive per task. Reporting one while meaning the other is how a
benchmark ends up describing lock contention as scheduler overhead.

---

## Research methodology

The project exists to answer *when* and *why* task-based execution helps, not to assert that it
does. Three properties make the numbers trustworthy, and all three are requirements rather than
aspirations:

**1. Measurements come from a headless, CPU-bound workload.** A renderer in the loop makes the
GPU the bottleneck, at which point both runtimes report the same frame time and the comparison
measures nothing. The flagship benchmark is therefore a headless fixed-timestep simulation with
no window and a seeded RNG. Rendering, when it exists, is visualization and sits outside the
measured path.

**2. Three comparison legs, not two.** StandardRuntime is a *competent* baseline, not a straw
man — but "it beat my own baseline" is a weak claim, so results are also compared against an
established industrial runtime (oneTBB / Taskflow), linked into the benchmark target only and
never into the runtime core.

**3. The measurement protocol accounts for the hardware.** The reference machine is a 15 W
mobile CPU whose sustained clocks drift under load by more than the effect being measured. Runs
are therefore interleaved A/B/A/B rather than batched, repeated at least 20 times, reported as
median and IQR rather than mean, and annotated with the observed clock frequency so throttled
runs are visible instead of silently averaged in.

Determinism is used as the correctness proof: the simulation hashes its full world state, and
that hash must be **bit-identical** across runtimes and across worker counts. This catches
scheduler races that stress tests miss — which matters here, because ThreadSanitizer is not
available on this toolchain.

---

## Limitations

Stated plainly, because they bound what this project can currently claim.

- **No ThreadSanitizer.** TSan is clang/Linux-only and unavailable under MSVC. ASan finds
  memory errors, *not* data races, and does not close this gap. Race confidence currently rests
  on the determinism hash, randomized stress tests, and choosing the simpler bounded deque over
  a growable one. This is a real residual risk and is not treated as solved.
- **Reference hardware is a 4-core / 8-thread 15 W laptop CPU.** Scaling studies stop at 8
  workers, and the efficiency drop past 4 is SMT rather than a scheduler defect. Results are
  labelled accordingly.
- **The floating-point determinism guarantee is conditional.** It holds only while every
  reduction is deterministically ordered and the pinned FP flags are unchanged.
- **The simulation is not a game.** It is a workload.

---

## Roadmap

Committed: task core and baseline runtime, then worker pool and work stealing, dependencies,
profiler and benchmark harness, headless simulation, adaptive scheduling modes, results.

Beyond that — renderer, streamed world, vehicles, aircraft, weather — is a genuine multi-year
scope and is treated as a roadmap rather than a backlog. Scheduling claims will continue to come
from the headless path regardless of how far the visual side progresses.

---

## License

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE).

Chosen over MIT for its **explicit patent grant** (section 3). That matters more than usual here:
a task scheduler is the kind of systems code where patents exist, and Apache 2.0 both grants
contributors' patent rights to users and terminates that grant for anyone who sues over them. MIT
is silent on patents.

### Third-party code

Taskflow (v3.7.0, MIT) is used **only as a benchmark reference**. It is fetched at configure time
behind `-DTHUNDERBOLT_REFERENCE_RUNTIMES=ON`, is never vendored into this repository, and links
only into the benchmark executables — nothing under `thunderbolt/` or `engine/` depends on it, and
a default build downloads nothing.

No third-party game assets, vehicle brands, or aircraft trademarks are used. Any vehicles or
aircraft are fictional and original.

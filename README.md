# Thunderbolt HT

**A high-performance parallel CPU execution runtime**, built around fine-grained task
scheduling, dependency-aware execution, work stealing and CPU-aware workload management —
together with a real-time simulation used as its flagship benchmark workload.

> **Thunderbolt HT does not create physical CPU cores and is not Intel Hyper-Threading.
> It is a software parallel execution runtime.** It exposes and exploits the CPU execution
> resources a machine already has; it does not add any.

---

## Status

**Phases A–H complete.** Read this section before any other — the rest of this document
describes the design, and this section describes what actually exists today.

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
| Results report — 6 of 8 §59 questions answered | ✅ [docs/RESULTS.md](docs/RESULTS.md) |
| Renderer, world, vehicles, aircraft | ⬜ roadmap |

119 unit tests pass under Debug, Release and AddressSanitizer. **28 of them are conformance
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

| leg | per-task cost | IQR at 65 536 tasks |
|---|---|---|
| standard | 5282 ns | 12% |
| **thunderbolt** | **985 ns** | 4% |
| taskflow, explicit tasks | 404 ns | 6% |
| taskflow, native `for_each` | 8 ns | 27% |

**Thunderbolt is ~2.4× behind Taskflow** on the like-for-like comparison. It is ~5.4× cheaper per
task than this project's own baseline — a real result, and a much weaker claim than it sounds,
which is exactly why the external leg exists.

The crossover — where decomposing further costs more than it buys — sits near **1000 tasks** for
this workload on this machine. That answers §59.6 and §59.7 directly, and needed no game.

### The simulation workload, and the result it produced

`thunderbolt-sim` runs a headless, fixed-timestep (60 Hz), seeded simulation: vehicles with an
engine/tyre/suspension model, NPCs with perception → decision → movement, and aircraft with
atmosphere → aerodynamics → propulsion → integration. Ten stages per tick across three
concurrently-running chains, all state structure-of-arrays and double-buffered.

The **same ten-stage graph** is also expressed with Taskflow (`game_benchmarks/TaskflowSim.cpp`,
benchmark target only) so the strongest claim in the project is checked against an industrial
scheduler rather than only against our own baseline. Both graphs produce an identical determinism
hash, which is what proves they express the same dependencies.

`stress` scene (500 vehicles, 2000 NPCs, 40 aircraft), 200 ticks, 8 workers, run through the
**same harness** as the synthetic benchmarks — interleaved A/B/A/B, warmup discarded, median and
IQR, throttle flagging, and a check that every leg produced the same world state:

```
thunderbolt-sim --scene stress --ticks 200 -w 8                 --ab serial,standard,thunderbolt,taskflow --reps 6 --out sim.json
```

| leg | ms/tick | IQR | vs serial |
|---|---|---|---|
| serial | 1.693 | 0.025 | 1.00× |
| standard | 1.429 | 0.203 | 1.18× |
| thunderbolt | 0.886 | 0.039 | 1.91× |
| **taskflow** | **0.672** | 0.078 | **2.52×** |

**Taskflow is ~1.32× faster than Thunderbolt** on a realistic frame graph. Much closer than the
~4× gap on the synthetic benchmark, but still ahead: Thunderbolt is a credible scheduler here and
not yet a competitive one.

**StandardRuntime gets only 1.18× from 8 workers.** Its single mutex-guarded queue absorbs almost
the entire benefit. That is the clearest evidence in the project for per-worker deques, and it
matches the synthetic scaling run where Thunderbolt only pulled ahead past 4 workers.

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
**~2.4×, not the ~4× previously reported**. The tool's default was already 20; the runs were
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

- **No timeline profiler (§54).** Only aggregate counters; there is no per-task span capture, which
  is the tool most likely to explain the remaining 2.4× gap to Taskflow.
- **oneTBB was not added.** Taskflow already serves as the external reference and a second heavy
  dependency would add build cost for little extra insight. A deliberate omission, not an oversight.
- **The workload-weight gate is advisory.** `T₁` is emitted and a warning printed for a scene
  too cheap to be a scheduling benchmark, but nothing refuses to print the speedup.
- Per-task cost is still ~4× an industrial scheduler on the synthetic benchmark, and the cause is
  not yet identified. The pool-mutex hypothesis was disproved; memory footprint is untested.

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
graph lets the runtime run whatever is legal to run, whenever a core is free â€” and makes the
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
enforced by tests (`architecture_layering`, `thunderbolt_builds_standalone`), not by convention â€”
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
| `asan` | AddressSanitizer. **Finds memory errors, not data races** â€” see Limitations. |
| `ninja` | Faster iteration; requires a Developer Command Prompt for `INCLUDE`/`LIB`. |

Add `-DTHUNDERBOLT_REFERENCE_RUNTIMES=ON` at configure time to fetch Taskflow and include the
external comparison legs. It is off by default: a core build must never require the network.

To verify the runtime really is independent of the simulation:

```
cmake -S thunderbolt -B build/standalone
```

To confirm CPU topology detection actually queried the OS rather than taking its fallback path
â€” the unit tests tolerate the fallback, so this is the check a human runs on real hardware:

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
task goes through the shared injection queue â€” a real pattern, but lock-bound at high task
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
man â€” but "it beat my own baseline" is a weak claim, so results are also compared against an
established industrial runtime (oneTBB / Taskflow), linked into the benchmark target only and
never into the runtime core.

**3. The measurement protocol accounts for the hardware.** The reference machine is a 15 W
mobile CPU whose sustained clocks drift under load by more than the effect being measured. Runs
are therefore interleaved A/B/A/B rather than batched, repeated at least 20 times, reported as
median and IQR rather than mean, and annotated with the observed clock frequency so throttled
runs are visible instead of silently averaged in.

Determinism is used as the correctness proof: the simulation hashes its full world state, and
that hash must be **bit-identical** across runtimes and across worker counts. This catches
scheduler races that stress tests miss â€” which matters here, because ThreadSanitizer is not
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

Beyond that â€” renderer, streamed world, vehicles, aircraft, weather â€” is a genuine multi-year
scope and is treated as a roadmap rather than a backlog. Scheduling claims will continue to come
from the headless path regardless of how far the visual side progresses.

---

## License

MIT â€” see [LICENSE](LICENSE).

No third-party game assets, vehicle brands, or aircraft trademarks are used. Any vehicles or
aircraft are fictional and original.

# Thunderbolt HT

**A high-performance parallel CPU execution runtime**, built around fine-grained task
scheduling, dependency-aware execution, work stealing and CPU-aware workload management —
together with a real-time simulation used as its flagship benchmark workload.

> **Thunderbolt HT does not create physical CPU cores and is not Intel Hyper-Threading.
> It is a software parallel execution runtime.** It exposes and exploits the CPU execution
> resources a machine already has; it does not add any.

---

## Status

**Phase E complete.** Read this section before any other — the rest of this document
describes the design, and this section describes what actually exists today.

| | |
|---|---|
| Build system, toolchain pinning, three configurations | ✅ built and verified |
| Architectural guard tests (layering, standalone build) | ✅ built and verified |
| Task core, `ITaskRuntime`, StandardRuntime | ✅ built and verified |
| Worker pool, work-stealing deque, CPU topology | ✅ built and verified |
| Task dependencies / task graph | ✅ built and verified |
| Profiler counters, benchmark harness, first results | ✅ built and measured |
| Headless deterministic simulation | ⬜ Phase F |
| Adaptive scheduling modes | ⬜ Phase G |
| Renderer, world, vehicles, aircraft | ⬜ roadmap |

105 unit tests pass under Debug, Release and AddressSanitizer. **28 of them are conformance
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

**Compared against an external reference.** Taskflow v3.7.0 is linked into the benchmark target
only — never into `thunderbolt/` — and run two ways, because handing it explicit tasks compares
like with like while its native `for_each` partitions the range itself and is its best case.
Reporting only the first would make it look bad for reasons unrelated to scheduling quality.

Total time for a fixed 2M-unit workload split into 65 536 tasks:

| leg | time | relative |
|---|---|---|
| standard (this project's baseline) | 0.307 s | 1.0× |
| **thunderbolt** | **0.118 s** | 2.6× |
| taskflow, explicit tasks | 0.045 s | 6.8× |
| taskflow, native `for_each` | 0.005 s | 61× |

**Thunderbolt is roughly 4× slower per task than an industrial scheduler on a like-for-like
comparison.** That is the honest headline, and it is exactly why the external leg was added:
"beats my own baseline" was true and nearly meaningless.

**Task granularity** (`docs/results/granularity.json`): per-task cost, recovered as the slope of
total time against task count with total work held constant, is ~4700 ns for the baseline and
~1750 ns for Thunderbolt (R² 0.99). The crossover — where decomposing further costs more than it
buys — sits near **1000 tasks** for this workload. That answers §59.6 and §59.7, and needed no
game.

**Scaling** (`docs/results/scaling.json`): near-linear to 4 physical cores, with efficiency
falling across workers 5–8 because those share cores via SMT — the output labels that boundary
so it is not misread as a scheduling defect. *Caveat:* some runs show efficiency slightly above
1.0 at 3–5 workers. That is not real superlinear scaling; it reflects turbo behaviour and a
shrinking per-core working set, and it is reported rather than smoothed away.

### A hypothesis that was tested and disproved

The task pool's free-list mutex was the standing explanation for high per-task cost — it is taken
twice per task, and instrumentation showed **52.8% of acquisitions contended** at 65 536 tasks.

Sharding the free list across 16 independent sublists reduced contention to **1.3%** and changed
the measured per-task cost **not at all** (~1750 ns before and after). The contention was real
but not causal: it was a symptom of workers being serialised somewhere else, not the cause.

The sharding was kept — it is strictly better and costs nothing — but it is recorded here as a
negative result rather than presented as an optimisation. The next candidate is memory
footprint: `sizeof(Task)` is 192 bytes, so a 65 536-task live set spans ~12 MB against roughly
6 MB of last-level cache. That is a hypothesis, not a finding.

### What is still NOT measured

- No timeline profiler (§54) yet — only aggregate counters.
- No game workload. Everything above is synthetic and CPU-bound by construction.
- oneTBB has not been added; the external reference is Taskflow alone.

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

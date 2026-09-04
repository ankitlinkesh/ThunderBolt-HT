# Thunderbolt HT

**A high-performance parallel CPU execution runtime**, built around fine-grained task
scheduling, dependency-aware execution, work stealing and CPU-aware workload management —
together with a real-time simulation used as its flagship benchmark workload.

> **Thunderbolt HT does not create physical CPU cores and is not Intel Hyper-Threading.
> It is a software parallel execution runtime.** It exposes and exploits the CPU execution
> resources a machine already has; it does not add any.

---

## Status

**Phase B complete.** Read this section before any other — the rest of this document
describes the design, and this section describes what actually exists today.

| | |
|---|---|
| Build system, toolchain pinning, three configurations | ✅ built and verified |
| Architectural guard tests (layering, standalone build) | ✅ built and verified |
| Task core, `ITaskRuntime`, StandardRuntime | ✅ built and verified |
| Worker pool, work-stealing deque | ⬜ Phase C |
| Task dependencies / task graph | ⬜ Phase D |
| Profiler, benchmark harness | ⬜ Phase E |
| Headless deterministic simulation | ⬜ Phase F |
| Adaptive scheduling modes | ⬜ Phase G |
| Renderer, world, vehicles, aircraft | ⬜ roadmap |

37 unit tests cover the task core and the baseline runtime, and pass under Debug, Release and
AddressSanitizer. Because ThreadSanitizer is unavailable here, the concurrency tests are also
run repeatedly rather than once: 75 consecutive clean runs across the three configurations at
the time of writing. That is weaker evidence than a race detector and is treated as such.

**No performance numbers are published yet, because none have been measured.** Every figure
that ever appears in this README will be traceable to a machine-readable result file produced
on stated hardware. There are no placeholder benchmarks in this repository.

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

To verify the runtime really is independent of the simulation:

```
cmake -S thunderbolt -B build/standalone
```

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

MIT — see [LICENSE](LICENSE).

No third-party game assets, vehicle brands, or aircraft trademarks are used. Any vehicles or
aircraft are fictional and original.

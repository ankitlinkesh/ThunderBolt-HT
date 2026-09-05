// The simulation frame graph, expressed with Taskflow.
//
// WHY THIS EXISTS. Thunderbolt's strongest measured result is on this workload -
// 2.2x over the in-house baseline at 8 workers. But that baseline is the same one
// Taskflow already beats by roughly 4x on the synthetic benchmark, so a win over
// it is exactly the straw-man comparison the external reference leg was added to
// prevent. The claim is only worth anything if the same frame graph, on the same
// state, is also run by an industrial scheduler.
//
// WHERE IT LIVES. In the benchmark target, not in engine/. Taskflow is a
// benchmark-only dependency; putting it behind engine/ would break the one-way
// layering the architecture test enforces. This file reaches into the simulation
// through Simulation::next_state() / finish_tick() and calls the same
// runtime-agnostic system functions from engine/core/Systems.hpp, so the two
// graphs differ ONLY in who schedules them.
//
// The determinism hash must match the Thunderbolt configurations exactly. That is
// not just a correctness check here: it proves both graphs express the same
// dependencies, without which the timing comparison would be meaningless.
#pragma once

#if THUNDERBOLT_HAVE_TASKFLOW

#include <engine/core/Simulation.hpp>

namespace tf {
class Executor;
}

namespace tbworld {

// Advances one fixed timestep using `executor`, then swaps buffers.
void tick_with_taskflow(Simulation& simulation, tf::Executor& executor);

} // namespace tbworld

#endif // THUNDERBOLT_HAVE_TASKFLOW

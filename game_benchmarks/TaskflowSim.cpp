#include "TaskflowSim.hpp"

#if THUNDERBOLT_HAVE_TASKFLOW

#include <engine/core/Systems.hpp>
#include <engine/core/World.hpp>

#include <taskflow/algorithm/for_each.hpp>
#include <taskflow/taskflow.hpp>

#include <cstddef>

namespace tbworld {
namespace {

// One pipeline stage: a parallel loop over BATCHES, not over entities.
//
// Iterating batches rather than elements matters for fairness. The Thunderbolt
// path submits one task per kBatchSize entities; handing Taskflow one iteration
// per entity would compare different granularities and quietly attribute the
// difference to scheduling. Taskflow still chooses how to partition the batch
// range among its workers, which is its native mode.
template <typename Fn>
tf::Task add_stage(tf::Taskflow& flow, std::size_t entity_count, Fn body) {
    const std::size_t batches = batch_count_for(entity_count);
    if (batches == 0) {
        return flow.placeholder();  // empty stage; still a graph node so edges hold
    }
    return flow.for_each_index(std::size_t{0}, batches, std::size_t{1},
                               [body, entity_count](std::size_t b) {
                                   const std::size_t begin = b * kBatchSize;
                                   const std::size_t end =
                                       (begin + kBatchSize < entity_count) ? (begin + kBatchSize)
                                                                           : entity_count;
                                   body(begin, end);
                               });
}

} // namespace

void tick_with_taskflow(Simulation& simulation, tf::Executor& executor) {
    const WorldState& in  = simulation.world().current();
    WorldState&       out = simulation.next_state();

    out.player_position = in.player_position;

    // Rebuilt every tick, matching what the Thunderbolt path does. Building it
    // once and re-running would be faster for Taskflow, but the two runtimes
    // would then be doing different amounts of work and the comparison would
    // measure graph construction rather than scheduling.
    tf::Taskflow flow;

    // The same three chains, with the same ten stages and the same edges as
    // Simulation::tick(). Any divergence here would show up as a determinism
    // hash mismatch before it could show up as a timing difference.
    tf::Task vehicle_lod = add_stage(flow, in.vehicles.size(), [&in, &out](std::size_t b,
                                                                          std::size_t e) {
        update_vehicle_lod(in, out, b, e);
    });
    tf::Task vehicle_physics =
        add_stage(flow, in.vehicles.size(),
                  [&in, &out](std::size_t b, std::size_t e) { update_vehicle_physics(in, out, b, e); });
    vehicle_lod.precede(vehicle_physics);

    tf::Task npc_lod = add_stage(
        flow, in.npcs.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_npc_lod(in, out, b, e); });
    tf::Task npc_perception = add_stage(
        flow, in.npcs.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_npc_perception(in, out, b, e); });
    tf::Task npc_decision = add_stage(
        flow, in.npcs.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_npc_decision(in, out, b, e); });
    tf::Task npc_movement = add_stage(
        flow, in.npcs.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_npc_movement(in, out, b, e); });
    npc_lod.precede(npc_perception);
    npc_perception.precede(npc_decision);
    npc_decision.precede(npc_movement);

    tf::Task air_atmosphere = add_stage(
        flow, in.aircraft.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_aircraft_atmosphere(in, out, b, e); });
    tf::Task air_aero = add_stage(
        flow, in.aircraft.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_aircraft_aerodynamics(in, out, b, e); });
    tf::Task air_propulsion = add_stage(
        flow, in.aircraft.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_aircraft_propulsion(in, out, b, e); });
    tf::Task air_integrate = add_stage(
        flow, in.aircraft.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_aircraft_integrate(in, out, b, e); });
    air_atmosphere.precede(air_aero);
    air_aero.precede(air_propulsion);
    air_propulsion.precede(air_integrate);

    executor.run(flow).wait();

    simulation.finish_tick();
}

} // namespace tbworld

#endif // THUNDERBOLT_HAVE_TASKFLOW

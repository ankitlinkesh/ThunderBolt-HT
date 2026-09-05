#include "Simulation.hpp"

#include "StateHash.hpp"
#include "Systems.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace tbworld {
namespace {

using thunderbolt::ITaskRuntime;
using thunderbolt::TaskHandle;

// S56's game_benchmarks scenes. Fixed here rather than passed on the command line
// so that a named result always refers to the same workload.
constexpr SceneSpec kScenes[] = {
    {"100_npcs",        0,  100,  0},
    {"500_npcs",        0,  500,  0},
    {"1000_npcs",       0, 1000,  0},
    {"50_vehicles",    50,    0,  0},
    {"100_vehicles",  100,    0,  0},
    {"250_vehicles",  250,    0,  0},
    {"5_aircraft",      0,    0,  5},
    {"20_aircraft",     0,    0, 20},
    {"full_mixed",    250, 1000, 20},
    {"stress",        500, 2000, 40},
};

// Submits one stage as a set of batches, each depending on `gate`, and returns a
// single handle that is complete only when every batch is.
//
// The barrier matters for cost, not just tidiness. Without it, each batch of the
// next stage would need an edge to EVERY batch of this one - quadratic in batch
// count, and at 64 batches that is 4096 dependency registrations per stage
// boundary, which would be measured as scheduler overhead.
template <typename Fn>
TaskHandle submit_stage(ITaskRuntime& runtime, std::vector<TaskHandle>& scratch, TaskHandle gate,
                        std::size_t entity_count, Fn&& body, std::uint64_t& submitted) {
    if (entity_count == 0) {
        return gate;  // nothing to do; downstream stages chain to the same gate
    }

    const std::size_t batches = batch_count_for(entity_count);

    scratch.clear();
    scratch.reserve(batches);

    for (std::size_t b = 0; b < batches; ++b) {
        const std::size_t begin = b * kBatchSize;
        const std::size_t end   = (begin + kBatchSize < entity_count) ? (begin + kBatchSize)
                                                                     : entity_count;

        // Batch bounds are computed from entity_count only - never from the
        // worker count - so the partition, and therefore the arithmetic, is
        // identical at any -w.
        if (gate.valid()) {
            scratch.push_back(runtime.submit_after({gate}, [body, begin, end] {
                body(begin, end);
            }));
        } else {
            scratch.push_back(runtime.submit([body, begin, end] { body(begin, end); }));
        }
        ++submitted;
    }

    TaskHandle barrier = runtime.submit_after(scratch, thunderbolt::TaskDesc{
                                                           thunderbolt::TaskFunction{[] {}}});
    ++submitted;
    return barrier;
}

} // namespace

const SceneSpec* find_scene(const char* name) noexcept {
    for (const SceneSpec& scene : kScenes) {
        if (std::strcmp(scene.name, name) == 0) {
            return &scene;
        }
    }
    return nullptr;
}

void list_scenes() {
    std::printf("scenes:\n");
    for (const SceneSpec& scene : kScenes) {
        std::printf("  %-14s vehicles=%-5zu npcs=%-5zu aircraft=%zu\n", scene.name, scene.vehicles,
                    scene.npcs, scene.aircraft);
    }
}

Simulation::Simulation(SceneSpec scene, std::uint64_t seed)
    : scene_(scene), world_(scene.vehicles, scene.npcs, scene.aircraft, seed) {}

void Simulation::tick(ITaskRuntime& runtime) {
    const WorldState& in  = world_.current();
    WorldState&       out = world_.next();

    // The player is simulated on the main thread: it is a single entity, and
    // making it a task would cost more than it saves.
    out.player_position = in.player_position;

    last_frame_ = FrameStats{};
    std::vector<TaskHandle> scratch;

    // Three independent chains, each with real internal ordering. They run
    // concurrently with one another, which is where the frame's width comes from;
    // the depth comes from the stages inside each chain.
    //
    //   vehicles : lod -> physics
    //   npcs     : lod -> perception -> decision -> movement
    //   aircraft : atmosphere -> aerodynamics -> propulsion -> integrate

    TaskHandle vehicle_gate = submit_stage(
        runtime, scratch, TaskHandle{}, in.vehicles.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_vehicle_lod(in, out, b, e); },
        last_frame_.tasks_submitted);
    vehicle_gate = submit_stage(
        runtime, scratch, vehicle_gate, in.vehicles.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_vehicle_physics(in, out, b, e); },
        last_frame_.tasks_submitted);

    TaskHandle npc_gate = submit_stage(
        runtime, scratch, TaskHandle{}, in.npcs.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_npc_lod(in, out, b, e); },
        last_frame_.tasks_submitted);
    npc_gate = submit_stage(
        runtime, scratch, npc_gate, in.npcs.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_npc_perception(in, out, b, e); },
        last_frame_.tasks_submitted);
    npc_gate = submit_stage(
        runtime, scratch, npc_gate, in.npcs.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_npc_decision(in, out, b, e); },
        last_frame_.tasks_submitted);
    npc_gate = submit_stage(
        runtime, scratch, npc_gate, in.npcs.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_npc_movement(in, out, b, e); },
        last_frame_.tasks_submitted);

    TaskHandle air_gate = submit_stage(
        runtime, scratch, TaskHandle{}, in.aircraft.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_aircraft_atmosphere(in, out, b, e); },
        last_frame_.tasks_submitted);
    air_gate = submit_stage(
        runtime, scratch, air_gate, in.aircraft.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_aircraft_aerodynamics(in, out, b, e); },
        last_frame_.tasks_submitted);
    air_gate = submit_stage(
        runtime, scratch, air_gate, in.aircraft.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_aircraft_propulsion(in, out, b, e); },
        last_frame_.tasks_submitted);
    air_gate = submit_stage(
        runtime, scratch, air_gate, in.aircraft.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_aircraft_integrate(in, out, b, e); },
        last_frame_.tasks_submitted);

    last_frame_.stages = 10;

    // Wait on the three chain tails rather than wait_all(): this is a frame
    // barrier for THIS frame's work, and wait_all() would also absorb anything
    // another thread happened to submit.
    runtime.wait(vehicle_gate);
    runtime.wait(npc_gate);
    runtime.wait(air_gate);

    world_.swap_buffers();
    world_.advance_tick();
}

void Simulation::tick_serial() {
    const WorldState& in  = world_.current();
    WorldState&       out = world_.next();

    out.player_position = in.player_position;

    // Identical order and identical batch bounds as the parallel path. Running
    // the same batches serially is what makes the serial result comparable to
    // the parallel one bit-for-bit rather than merely approximately.
    auto run_batched = [](std::size_t count, auto&& body) {
        for (std::size_t b = 0; b < batch_count_for(count); ++b) {
            const std::size_t begin = b * kBatchSize;
            const std::size_t end   = (begin + kBatchSize < count) ? (begin + kBatchSize) : count;
            body(begin, end);
        }
    };

    run_batched(in.vehicles.size(),
                [&](std::size_t b, std::size_t e) { update_vehicle_lod(in, out, b, e); });
    run_batched(in.vehicles.size(),
                [&](std::size_t b, std::size_t e) { update_vehicle_physics(in, out, b, e); });

    run_batched(in.npcs.size(),
                [&](std::size_t b, std::size_t e) { update_npc_lod(in, out, b, e); });
    run_batched(in.npcs.size(),
                [&](std::size_t b, std::size_t e) { update_npc_perception(in, out, b, e); });
    run_batched(in.npcs.size(),
                [&](std::size_t b, std::size_t e) { update_npc_decision(in, out, b, e); });
    run_batched(in.npcs.size(),
                [&](std::size_t b, std::size_t e) { update_npc_movement(in, out, b, e); });

    run_batched(in.aircraft.size(),
                [&](std::size_t b, std::size_t e) { update_aircraft_atmosphere(in, out, b, e); });
    run_batched(in.aircraft.size(),
                [&](std::size_t b, std::size_t e) { update_aircraft_aerodynamics(in, out, b, e); });
    run_batched(in.aircraft.size(),
                [&](std::size_t b, std::size_t e) { update_aircraft_propulsion(in, out, b, e); });
    run_batched(in.aircraft.size(),
                [&](std::size_t b, std::size_t e) { update_aircraft_integrate(in, out, b, e); });

    world_.swap_buffers();
    world_.advance_tick();
}

std::uint64_t Simulation::state_hash() const { return hash_state(world_.current()); }

} // namespace tbworld

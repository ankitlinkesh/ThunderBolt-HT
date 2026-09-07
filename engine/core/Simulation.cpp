#include "Simulation.hpp"

#include "StateHash.hpp"
#include "Systems.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
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

// Submits one stage as at most one task PER WORKER, each claiming batches from a
// shared cursor, and returns a single handle complete only when all of them are.
//
// This used to submit one task per BATCH - at kBatchSize=32, the `stress` scene's
// 2000 NPCs meant 63 discrete tasks for a single stage, ten times over, ~300
// tasks a tick. Taskflow's equivalent (game_benchmarks/TaskflowSim.cpp) submits
// ONE graph node per stage and lets its own partitioner fan it out internally -
// the same one-task-per-batch-vs-dynamic-partitioning gap Stage 6 already fixed
// for parallel_for, just never applied here. Same fix, same shape: O(workers)
// tasks instead of O(batches), each looping on an atomic cursor over batch
// indices until the stage is exhausted.
//
// The barrier still matters for cost, not just tidiness: without it, the next
// stage's tasks would each need an edge to every claiming task of this one.
// Bounding the claiming task count by worker_count() rather than batch_count
// bounds the barrier's dependency-edge count the same way.
//
// `cursor_storage` owns the shared cursors for the whole tick(); a raw pointer
// into one of its elements is safe because they are heap-allocated
// (unique_ptr<atomic<...>>, so a vector reallocation moves the pointer, never
// the pointee) and the vector outlives every task submitted through it - tick()
// only lets it go out of scope after waiting on all three chain tails.
template <typename Fn>
TaskHandle submit_stage(ITaskRuntime& runtime, std::vector<TaskHandle>& scratch,
                        std::vector<std::unique_ptr<std::atomic<std::size_t>>>& cursor_storage,
                        TaskHandle gate, std::size_t entity_count, Fn&& body,
                        std::uint64_t& submitted) {
    if (entity_count == 0) {
        return gate;  // nothing to do; downstream stages chain to the same gate
    }

    const std::size_t   batches = batch_count_for(entity_count);
    const std::uint32_t configured_workers = runtime.worker_count();
    const std::size_t   worker_cap = (configured_workers == 0) ? 1 : configured_workers;
    const std::size_t   claimants  = (batches < worker_cap) ? batches : worker_cap;

    cursor_storage.push_back(std::make_unique<std::atomic<std::size_t>>(0));
    std::atomic<std::size_t>* cursor = cursor_storage.back().get();

    // Batch COUNT and boundaries are computed from entity_count only - never
    // from the worker count - so which claimant ends up running a given batch
    // may vary with -w, but the set of batches, and therefore the result, does
    // not. Every system this runs is element-wise (S51's rule), so that is
    // sufficient for the determinism hash to stay identical across worker
    // counts; a REDUCTION dispatched this way would not be safe.
    auto claim_batches = [body, cursor, batches, entity_count] {
        for (;;) {
            const std::size_t b = cursor->fetch_add(1, std::memory_order_relaxed);
            if (b >= batches) {
                break;
            }
            const std::size_t begin = b * kBatchSize;
            const std::size_t end =
                (begin + kBatchSize < entity_count) ? (begin + kBatchSize) : entity_count;
            body(begin, end);
        }
    };

    scratch.clear();
    scratch.reserve(claimants);

    for (std::size_t c = 0; c < claimants; ++c) {
        if (gate.valid()) {
            scratch.push_back(runtime.submit_after({gate}, claim_batches));
        } else {
            scratch.push_back(runtime.submit(claim_batches));
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

    // Owns this frame's per-stage claim cursors. Sized to the ten stages below
    // so no stage's push_back needs to grow it, though correctness does not
    // depend on that - see submit_stage's comment on why a reallocation here
    // cannot invalidate a pointer a task is already holding.
    std::vector<std::unique_ptr<std::atomic<std::size_t>>> cursor_storage;
    cursor_storage.reserve(10);

    // Three independent chains, each with real internal ordering. They run
    // concurrently with one another, which is where the frame's width comes from;
    // the depth comes from the stages inside each chain.
    //
    //   vehicles : lod -> physics
    //   npcs     : lod -> perception -> decision -> movement
    //   aircraft : atmosphere -> aerodynamics -> propulsion -> integrate

    TaskHandle vehicle_gate = submit_stage(
        runtime, scratch, cursor_storage, TaskHandle{}, in.vehicles.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_vehicle_lod(in, out, b, e); },
        last_frame_.tasks_submitted);
    vehicle_gate = submit_stage(
        runtime, scratch, cursor_storage, vehicle_gate, in.vehicles.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_vehicle_physics(in, out, b, e); },
        last_frame_.tasks_submitted);

    TaskHandle npc_gate = submit_stage(
        runtime, scratch, cursor_storage, TaskHandle{}, in.npcs.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_npc_lod(in, out, b, e); },
        last_frame_.tasks_submitted);
    npc_gate = submit_stage(
        runtime, scratch, cursor_storage, npc_gate, in.npcs.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_npc_perception(in, out, b, e); },
        last_frame_.tasks_submitted);
    npc_gate = submit_stage(
        runtime, scratch, cursor_storage, npc_gate, in.npcs.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_npc_decision(in, out, b, e); },
        last_frame_.tasks_submitted);
    npc_gate = submit_stage(
        runtime, scratch, cursor_storage, npc_gate, in.npcs.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_npc_movement(in, out, b, e); },
        last_frame_.tasks_submitted);

    TaskHandle air_gate = submit_stage(
        runtime, scratch, cursor_storage, TaskHandle{}, in.aircraft.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_aircraft_atmosphere(in, out, b, e); },
        last_frame_.tasks_submitted);
    air_gate = submit_stage(
        runtime, scratch, cursor_storage, air_gate, in.aircraft.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_aircraft_aerodynamics(in, out, b, e); },
        last_frame_.tasks_submitted);
    air_gate = submit_stage(
        runtime, scratch, cursor_storage, air_gate, in.aircraft.size(),
        [&in, &out](std::size_t b, std::size_t e) { update_aircraft_propulsion(in, out, b, e); },
        last_frame_.tasks_submitted);
    air_gate = submit_stage(
        runtime, scratch, cursor_storage, air_gate, in.aircraft.size(),
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

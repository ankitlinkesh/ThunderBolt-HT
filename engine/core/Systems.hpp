// Thunderbolt Open World - simulation systems.
//
// Every system below has the same shape: it reads the PREVIOUS frame, writes the
// NEXT one, and touches only entities in the half-open range it is given. Two
// consequences follow, and both are the point:
//
//   - No system can race another, because no two writes ever target the same
//     element and no read ever observes a partially written frame.
//   - The result is independent of how the range was divided, so the state hash
//     is identical at any worker count.
//
// Batch boundaries are a function of ENTITY COUNT ALONE (see kBatchSize). Deriving
// them from worker count would make the partition - and therefore any cross-batch
// arithmetic - vary with -w, and the determinism harness would fail for a reason
// that has nothing to do with a bug.
#pragma once

#include "World.hpp"

#include <cstddef>

namespace tbworld {

// Entities per batch. A fixed constant, deliberately NOT derived from the worker
// count or the hardware. Its value is a granularity tuning knob; its
// independence from scheduling is a correctness requirement.
inline constexpr std::size_t kBatchSize = 32;

[[nodiscard]] inline std::size_t batch_count_for(std::size_t entity_count) noexcept {
    return (entity_count + kBatchSize - 1) / kBatchSize;
}

// --- level of detail (S45) ------------------------------------------------
// Assigned from each entity's own distance to the player, so an entity's LOD
// depends on the world and never on which worker evaluated it.
void update_vehicle_lod(const WorldState& in, WorldState& out, std::size_t begin,
                        std::size_t end) noexcept;
void update_npc_lod(const WorldState& in, WorldState& out, std::size_t begin,
                    std::size_t end) noexcept;

// --- NPCs (S43) -----------------------------------------------------------
// Three stages with real data dependencies between them: perception writes the
// nearest-neighbour fields, decision reads them to steer, movement integrates.
// The frame graph must order them, which is what gives the graph depth.
void update_npc_perception(const WorldState& in, WorldState& out, std::size_t begin,
                           std::size_t end) noexcept;
void update_npc_decision(const WorldState& in, WorldState& out, std::size_t begin,
                         std::size_t end) noexcept;
void update_npc_movement(const WorldState& in, WorldState& out, std::size_t begin,
                         std::size_t end) noexcept;

// --- vehicles (S32) -------------------------------------------------------
// Engine torque, drivetrain, tyre slip, suspension load, aerodynamic drag, then
// integration at a fixed timestep.
void update_vehicle_physics(const WorldState& in, WorldState& out, std::size_t begin,
                            std::size_t end) noexcept;

// --- aircraft (S36, S42) --------------------------------------------------
// Decomposed the way the spec describes: atmosphere feeds aerodynamics and
// propulsion, both feed integration.
void update_aircraft_atmosphere(const WorldState& in, WorldState& out, std::size_t begin,
                                std::size_t end) noexcept;
void update_aircraft_aerodynamics(const WorldState& in, WorldState& out, std::size_t begin,
                                  std::size_t end) noexcept;
void update_aircraft_propulsion(const WorldState& in, WorldState& out, std::size_t begin,
                                std::size_t end) noexcept;
void update_aircraft_integrate(const WorldState& in, WorldState& out, std::size_t begin,
                               std::size_t end) noexcept;

// --- fused per-entity chains ----------------------------------------------
// Each chain is entity-local: every stage reads `in` (previous frame) for OTHER
// entities and reads `out` only at its OWN index, written by an earlier stage of
// the same chain (NPC perception reads in.npcs.position of neighbours, never
// out). So running the whole chain over one batch, stage after stage, computes
// exactly what ten barriered stages did - same kernels, same per-entity order -
// and needs no barrier inside the chain. Hash-identical by construction.
inline void update_vehicle_chain(const WorldState& in, WorldState& out, std::size_t b,
                                 std::size_t e) noexcept {
    update_vehicle_lod(in, out, b, e);
    update_vehicle_physics(in, out, b, e);
}
inline void update_npc_chain(const WorldState& in, WorldState& out, std::size_t b,
                             std::size_t e) noexcept {
    update_npc_lod(in, out, b, e);
    update_npc_perception(in, out, b, e);
    update_npc_decision(in, out, b, e);
    update_npc_movement(in, out, b, e);
}
inline void update_aircraft_chain(const WorldState& in, WorldState& out, std::size_t b,
                                  std::size_t e) noexcept {
    update_aircraft_atmosphere(in, out, b, e);
    update_aircraft_aerodynamics(in, out, b, e);
    update_aircraft_propulsion(in, out, b, e);
    update_aircraft_integrate(in, out, b, e);
}

} // namespace tbworld

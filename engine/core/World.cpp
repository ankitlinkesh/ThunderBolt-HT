#include "World.hpp"

#include <utility>

namespace tbworld {

void VehicleArrays::resize(std::size_t count) {
    position.assign(count, Vec3{});
    velocity.assign(count, Vec3{});
    heading.assign(count, 0.0f);
    steer.assign(count, 0.0f);
    throttle.assign(count, 0.0f);
    engine_rpm.assign(count, 0.0f);
    wheel_load.assign(count, 0.0f);
    mass.assign(count, 1500.0f);
    lod.assign(count, SimLod::Full);
    lane.assign(count, 0u);
}

void NpcArrays::resize(std::size_t count) {
    position.assign(count, Vec3{});
    velocity.assign(count, Vec3{});
    target.assign(count, Vec3{});
    speed.assign(count, 0.0f);
    behaviour.assign(count, 0u);
    lod.assign(count, SimLod::Full);
    nearest_distance.assign(count, 0.0f);
    nearest_index.assign(count, 0u);
}

void AircraftArrays::resize(std::size_t count) {
    position.assign(count, Vec3{});
    velocity.assign(count, Vec3{});
    heading.assign(count, 0.0f);
    pitch.assign(count, 0.0f);
    throttle.assign(count, 0.0f);
    altitude.assign(count, 0.0f);
    air_density.assign(count, 1.225f);
    lift.assign(count, 0.0f);
    drag.assign(count, 0.0f);
    thrust.assign(count, 0.0f);
    mass.assign(count, 60000.0f);
}

void WorldState::resize(std::size_t vehicle_count, std::size_t npc_count,
                        std::size_t aircraft_count) {
    vehicles.resize(vehicle_count);
    npcs.resize(npc_count);
    aircraft.resize(aircraft_count);
}

World::World(std::size_t vehicle_count, std::size_t npc_count, std::size_t aircraft_count,
             std::uint64_t seed)
    : seed_(seed) {
    buffers_[0].resize(vehicle_count, npc_count, aircraft_count);
    buffers_[1].resize(vehicle_count, npc_count, aircraft_count);
    seed_initial_state();
}

void World::seed_initial_state() {
    WorldState& state = buffers_[0];

    // Every initial value is derived from (seed, entity index). Nothing is drawn
    // from a shared stream, so the starting state is identical regardless of
    // worker count - which it must be, or the hash comparison would be testing
    // initialisation rather than simulation.
    for (std::size_t i = 0; i < state.vehicles.size(); ++i) {
        const auto index = static_cast<std::uint64_t>(i);
        state.vehicles.position[i] = Vec3{random_range(seed_, index, 0.0f, kWorldSize, 1),
                                          0.0f,
                                          random_range(seed_, index, 0.0f, kWorldSize, 2)};
        state.vehicles.heading[i]  = random_range(seed_, index, 0.0f, 6.2831853f, 3);
        state.vehicles.throttle[i] = random_range(seed_, index, 0.2f, 1.0f, 4);
        state.vehicles.mass[i]     = random_range(seed_, index, 900.0f, 2600.0f, 5);
        state.vehicles.lane[i]     = static_cast<std::uint32_t>(index % 4);
        state.vehicles.engine_rpm[i] = 800.0f;
    }

    for (std::size_t i = 0; i < state.npcs.size(); ++i) {
        const auto index = static_cast<std::uint64_t>(i);
        state.npcs.position[i] = Vec3{random_range(seed_, index, 0.0f, kWorldSize, 11),
                                      0.0f,
                                      random_range(seed_, index, 0.0f, kWorldSize, 12)};
        state.npcs.target[i]   = Vec3{random_range(seed_, index, 0.0f, kWorldSize, 13),
                                    0.0f,
                                    random_range(seed_, index, 0.0f, kWorldSize, 14)};
        state.npcs.speed[i]     = random_range(seed_, index, 0.8f, 2.2f, 15);
        state.npcs.behaviour[i] = static_cast<std::uint32_t>(index % 3);
    }

    for (std::size_t i = 0; i < state.aircraft.size(); ++i) {
        const auto index = static_cast<std::uint64_t>(i);
        state.aircraft.position[i] = Vec3{random_range(seed_, index, 0.0f, kWorldSize, 21),
                                         random_range(seed_, index, 300.0f, 9000.0f, 22),
                                         random_range(seed_, index, 0.0f, kWorldSize, 23)};
        state.aircraft.altitude[i] = state.aircraft.position[i].y;
        state.aircraft.heading[i]  = random_range(seed_, index, 0.0f, 6.2831853f, 24);
        state.aircraft.throttle[i] = random_range(seed_, index, 0.5f, 1.0f, 25);
        state.aircraft.mass[i]     = random_range(seed_, index, 1200.0f, 250000.0f, 26);
        state.aircraft.velocity[i] = Vec3{random_range(seed_, index, 60.0f, 240.0f, 27), 0.0f,
                                          0.0f};
    }

    state.player_position = Vec3{kWorldSize * 0.5f, 0.0f, kWorldSize * 0.5f};

    // The second buffer starts as a copy, so the first tick reads a fully
    // initialised previous frame rather than zeros.
    buffers_[1] = buffers_[0];
}

void World::swap_buffers() noexcept { std::swap(current_, next_); }

} // namespace tbworld

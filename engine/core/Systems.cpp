#include "Systems.hpp"

namespace tbworld {
namespace {

[[nodiscard]] SimLod lod_for_distance(float distance) noexcept {
    // S45's bands. Configurable in principle; fixed here so the workload is
    // comparable between runs.
    if (distance < 50.0f) {
        return SimLod::Full;
    }
    if (distance < 250.0f) {
        return SimLod::Reduced;
    }
    if (distance < 1000.0f) {
        return SimLod::Simple;
    }
    return SimLod::Distant;
}

// Physics sub-steps by LOD. A distant entity is integrated once; a nearby one
// several times. This is what makes LOD a real cost lever rather than a label.
[[nodiscard]] int substeps_for(SimLod lod) noexcept {
    switch (lod) {
    case SimLod::Full:    return 4;
    case SimLod::Reduced: return 2;
    case SimLod::Simple:  return 1;
    case SimLod::Distant: return 1;
    }
    return 1;
}

// How many neighbours perception examines. Fixed, so the cost per NPC is stable
// and the work does not depend on the spatial distribution - which would make
// timings vary between seeds for reasons unrelated to scheduling.
constexpr std::size_t kPerceptionWindow = 24;

} // namespace

void update_vehicle_lod(const WorldState& in, WorldState& out, std::size_t begin,
                        std::size_t end) noexcept {
    for (std::size_t i = begin; i < end; ++i) {
        const Vec3  offset   = sub(in.vehicles.position[i], in.player_position);
        const float distance = length(offset);
        out.vehicles.lod[i]  = lod_for_distance(distance);
    }
}

void update_npc_lod(const WorldState& in, WorldState& out, std::size_t begin,
                    std::size_t end) noexcept {
    for (std::size_t i = begin; i < end; ++i) {
        const Vec3  offset   = sub(in.npcs.position[i], in.player_position);
        const float distance = length(offset);
        out.npcs.lod[i]      = lod_for_distance(distance);
    }
}

void update_npc_perception(const WorldState& in, WorldState& out, std::size_t begin,
                           std::size_t end) noexcept {
    const std::size_t count = in.npcs.size();
    if (count == 0) {
        return;
    }

    for (std::size_t i = begin; i < end; ++i) {
        const Vec3 self = in.npcs.position[i];

        float         nearest_sq    = 1e30f;
        std::uint32_t nearest_index = static_cast<std::uint32_t>(i);

        // A fixed index window rather than a spatial query. Not a realistic
        // neighbourhood, but it is O(1) per NPC, deterministic, and reads only
        // the previous frame - which is what this workload needs. A real spatial
        // grid is a later concern; the scheduling shape is the same either way.
        for (std::size_t k = 1; k <= kPerceptionWindow; ++k) {
            const std::size_t j = (i + k) % count;

            const Vec3  offset      = sub(in.npcs.position[j], self);
            const float distance_sq = length_squared(offset);

            // Strictly-less comparison with a tie broken by the lower index, so
            // the winner does not depend on evaluation order.
            if (distance_sq < nearest_sq) {
                nearest_sq    = distance_sq;
                nearest_index = static_cast<std::uint32_t>(j);
            }
        }

        out.npcs.nearest_distance[i] = std::sqrt(nearest_sq);
        out.npcs.nearest_index[i]    = nearest_index;
    }
}

void update_npc_decision(const WorldState& in, WorldState& out, std::size_t begin,
                         std::size_t end) noexcept {
    for (std::size_t i = begin; i < end; ++i) {
        // Reads perception's output from the OUTPUT buffer, which is why the
        // frame graph must order perception before decision.
        const float nearest = out.npcs.nearest_distance[i];

        const Vec3 to_target = sub(in.npcs.target[i], in.npcs.position[i]);
        Vec3       heading   = normalized_or_zero(to_target);

        // Separation: steer away from a crowded neighbour.
        if (nearest < 3.0f) {
            const std::uint32_t other = out.npcs.nearest_index[i];
            const Vec3 away = normalized_or_zero(sub(in.npcs.position[i], in.npcs.position[other]));
            heading         = normalized_or_zero(add(scale(heading, 0.4f), scale(away, 0.6f)));
        }

        float speed = in.npcs.speed[i];
        if (in.npcs.behaviour[i] == 1u) {
            speed *= 1.35f;  // hurrying
        } else if (in.npcs.behaviour[i] == 2u) {
            speed *= 0.7f;   // strolling
        }

        // Arrival: slow down near the target rather than orbiting it.
        const float distance = length(to_target);
        if (distance < 5.0f) {
            speed *= clampf(distance * 0.2f, 0.1f, 1.0f);
        }

        out.npcs.velocity[i] = scale(heading, speed);
        out.npcs.speed[i]    = in.npcs.speed[i];
    }
}

void update_npc_movement(const WorldState& in, WorldState& out, std::size_t begin,
                         std::size_t end) noexcept {
    for (std::size_t i = begin; i < end; ++i) {
        const Vec3 velocity = out.npcs.velocity[i];
        Vec3       position = add(in.npcs.position[i], scale(velocity, kFixedTimestep));

        position.x = wrapf(position.x, kWorldSize);
        position.z = wrapf(position.z, kWorldSize);

        out.npcs.position[i] = position;

        // Pick a new target on arrival. Derived from (seed-free) tick-independent
        // entity state, so it stays reproducible.
        Vec3        target   = in.npcs.target[i];
        const float distance = length(sub(target, position));
        if (distance < 2.0f) {
            const auto index = static_cast<std::uint64_t>(i);
            target = Vec3{wrapf(target.z * 1.37f + 137.0f, kWorldSize), 0.0f,
                          wrapf(target.x * 1.19f + 71.0f, kWorldSize)};
            (void)index;
        }
        out.npcs.target[i]    = target;
        out.npcs.behaviour[i] = in.npcs.behaviour[i];
    }
}

void update_vehicle_physics(const WorldState& in, WorldState& out, std::size_t begin,
                            std::size_t end) noexcept {
    for (std::size_t i = begin; i < end; ++i) {
        const SimLod lod   = out.vehicles.lod[i];  // written by the LOD stage
        const int    steps = substeps_for(lod);

        Vec3  position = in.vehicles.position[i];
        Vec3  velocity = in.vehicles.velocity[i];
        float heading  = in.vehicles.heading[i];
        float rpm      = in.vehicles.engine_rpm[i];

        const float mass     = in.vehicles.mass[i];
        const float throttle = in.vehicles.throttle[i];
        const float dt       = kFixedTimestep / static_cast<float>(steps);

        for (int step = 0; step < steps; ++step) {
            const float speed = length(velocity);

            // Engine torque curve: rises then falls away past peak.
            const float normalized_rpm = clampf(rpm / 6500.0f, 0.0f, 1.2f);
            const float torque = 320.0f * throttle * (1.0f - (normalized_rpm - 0.55f) *
                                                                 (normalized_rpm - 0.55f) * 2.2f);

            // Drivetrain to wheel force, with a fixed final drive.
            const float wheel_force = clampf(torque, 0.0f, 400.0f) * 3.4f / 0.32f;

            // Tyre longitudinal slip, saturating at the friction limit.
            const float slip       = clampf(wheel_force / (mass * 9.81f), -1.0f, 1.0f);
            const float grip_force = slip * mass * 9.81f * 0.95f;

            // Aerodynamic drag and rolling resistance.
            const float drag    = 0.5f * 1.225f * 0.31f * 2.2f * speed * speed;
            const float rolling = 0.015f * mass * 9.81f;

            const float net_force   = grip_force - drag - rolling;
            const float acceleration = net_force / mass;

            heading += in.vehicles.steer[i] * dt * 0.6f;
            heading = wrapf(heading, 6.2831853f);

            const Vec3 forward{std::cos(heading), 0.0f, std::sin(heading)};
            velocity = add(velocity, scale(forward, acceleration * dt));

            // Lateral damping stands in for tyre side force.
            velocity = scale(velocity, 0.999f);

            position = add(position, scale(velocity, dt));

            // Suspension load transfer under acceleration.
            out.vehicles.wheel_load[i] = mass * 9.81f * 0.25f + acceleration * mass * 0.08f;

            rpm = clampf(std::abs(length(velocity)) * 3.4f / 0.32f * 9.549f, 800.0f, 7200.0f);
        }

        position.x = wrapf(position.x, kWorldSize);
        position.z = wrapf(position.z, kWorldSize);

        out.vehicles.position[i]   = position;
        out.vehicles.velocity[i]   = velocity;
        out.vehicles.heading[i]    = heading;
        out.vehicles.engine_rpm[i] = rpm;
        out.vehicles.throttle[i]   = throttle;
        out.vehicles.mass[i]       = mass;
        out.vehicles.lane[i]       = in.vehicles.lane[i];

        // Gentle steering oscillation, a function of lane and position only.
        out.vehicles.steer[i] =
            clampf(std::sin(position.x * 0.01f + static_cast<float>(in.vehicles.lane[i])) * 0.25f,
                   -0.5f, 0.5f);
    }
}

void update_aircraft_atmosphere(const WorldState& in, WorldState& out, std::size_t begin,
                                std::size_t end) noexcept {
    for (std::size_t i = begin; i < end; ++i) {
        const float altitude = in.aircraft.position[i].y;

        // ISA troposphere: temperature lapse, then density from the barometric
        // relation. Enough structure to cost something and to matter downstream.
        const float temperature = 288.15f - 0.0065f * altitude;
        const float ratio       = clampf(temperature / 288.15f, 0.1f, 1.5f);
        const float density     = 1.225f * std::pow(ratio, 4.2561f);

        out.aircraft.air_density[i] = density;
        out.aircraft.altitude[i]    = altitude;
    }
}

void update_aircraft_aerodynamics(const WorldState& in, WorldState& out, std::size_t begin,
                                  std::size_t end) noexcept {
    for (std::size_t i = begin; i < end; ++i) {
        // Reads the atmosphere stage's output: the ordering edge is real.
        const float density = out.aircraft.air_density[i];
        const Vec3  velocity = in.aircraft.velocity[i];
        const float speed    = length(velocity);

        const float wing_area   = 0.0006f * in.aircraft.mass[i] + 16.0f;
        const float angle       = in.aircraft.pitch[i];
        const float lift_coeff  = clampf(0.2f + 5.7f * angle, -1.6f, 1.6f);
        const float drag_coeff  = 0.021f + (lift_coeff * lift_coeff) / (3.1416f * 8.0f * 0.8f);

        const float dynamic_pressure = 0.5f * density * speed * speed;

        out.aircraft.lift[i] = dynamic_pressure * wing_area * lift_coeff;
        out.aircraft.drag[i] = dynamic_pressure * wing_area * drag_coeff;
    }
}

void update_aircraft_propulsion(const WorldState& in, WorldState& out, std::size_t begin,
                                std::size_t end) noexcept {
    for (std::size_t i = begin; i < end; ++i) {
        const float density  = out.aircraft.air_density[i];
        const float throttle = in.aircraft.throttle[i];

        // Thrust falls with density, which is why altitude limits climb rate.
        const float sea_level_thrust = in.aircraft.mass[i] * 0.30f;
        out.aircraft.thrust[i]       = sea_level_thrust * throttle * (density / 1.225f);
        out.aircraft.throttle[i]     = throttle;
    }
}

void update_aircraft_integrate(const WorldState& in, WorldState& out, std::size_t begin,
                               std::size_t end) noexcept {
    for (std::size_t i = begin; i < end; ++i) {
        const float mass  = in.aircraft.mass[i];
        const float lift  = out.aircraft.lift[i];
        const float drag  = out.aircraft.drag[i];
        const float thrust = out.aircraft.thrust[i];

        float      heading  = in.aircraft.heading[i];
        Vec3       velocity = in.aircraft.velocity[i];
        Vec3       position = in.aircraft.position[i];

        const Vec3  forward{std::cos(heading), 0.0f, std::sin(heading)};
        const float speed = length(velocity);

        const float along    = (thrust - drag) / mass;
        const float vertical = (lift - mass * 9.81f) / mass;

        velocity = add(velocity, scale(forward, along * kFixedTimestep));
        velocity.y += vertical * kFixedTimestep;
        velocity.y = clampf(velocity.y, -60.0f, 60.0f);

        position = add(position, scale(velocity, kFixedTimestep));

        // Keep aircraft airborne and inside the world.
        position.y = clampf(position.y, 150.0f, 12000.0f);
        position.x = wrapf(position.x, kWorldSize);
        position.z = wrapf(position.z, kWorldSize);

        // Gentle turn, a function of position alone.
        heading = wrapf(heading + std::sin(position.x * 0.0005f) * 0.004f, 6.2831853f);

        // Pitch toward level flight, proportional to the vertical imbalance.
        const float pitch = clampf(in.aircraft.pitch[i] - vertical * 0.0006f, -0.28f, 0.28f);

        out.aircraft.position[i] = position;
        out.aircraft.velocity[i] = velocity;
        out.aircraft.heading[i]  = heading;
        out.aircraft.pitch[i]    = pitch;
        out.aircraft.mass[i]     = mass;
        (void)speed;
    }
}

} // namespace tbworld

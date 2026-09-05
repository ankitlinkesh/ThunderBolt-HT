// Thunderbolt Open World - simulation state.
//
// Structure-of-arrays (S51, S18): each attribute is its own contiguous array, so
// a system that touches only positions streams positions and nothing else. The
// alternative - an array of fat entity structs - drags every unrelated field
// through cache on every pass, and the resulting stalls would be measured as
// scheduler overhead.
//
// DOUBLE BUFFERED (S69, S70). Systems read the previous frame and write the next.
// That is not only about avoiding locks: it is what makes the frame's result
// independent of the order in which entities are processed, and therefore
// independent of how the work was scheduled. With a single buffer, whether entity
// 7 sees entity 3's old or new position would depend on which worker got there
// first, and the state hash would differ between runs.
#pragma once

#include "Math.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tbworld {

// Simulation level of detail (S45). Assigned from distance to the player, so a
// distant entity costs less - and, importantly, is assigned per entity from its
// own position, never from a worker's view of the world.
enum class SimLod : std::uint8_t {
    Full     = 0,  // full physics
    Reduced  = 1,
    Simple   = 2,
    Distant  = 3,  // statistical only
};

struct VehicleArrays {
    std::vector<Vec3>  position;
    std::vector<Vec3>  velocity;
    std::vector<float> heading;
    std::vector<float> steer;
    std::vector<float> throttle;
    std::vector<float> engine_rpm;
    std::vector<float> wheel_load;   // stands in for suspension state
    std::vector<float> mass;
    std::vector<SimLod> lod;
    std::vector<std::uint32_t> lane;

    void resize(std::size_t count);
    [[nodiscard]] std::size_t size() const noexcept { return position.size(); }
};

struct NpcArrays {
    std::vector<Vec3>  position;
    std::vector<Vec3>  velocity;
    std::vector<Vec3>  target;
    std::vector<float> speed;
    std::vector<std::uint32_t> behaviour;
    std::vector<SimLod>        lod;
    // Written by perception, read by decision - a real inter-system dependency,
    // which is the point: it forces the frame graph to have edges.
    std::vector<float> nearest_distance;
    std::vector<std::uint32_t> nearest_index;

    void resize(std::size_t count);
    [[nodiscard]] std::size_t size() const noexcept { return position.size(); }
};

struct AircraftArrays {
    std::vector<Vec3>  position;
    std::vector<Vec3>  velocity;
    std::vector<float> heading;
    std::vector<float> pitch;
    std::vector<float> throttle;
    std::vector<float> altitude;
    std::vector<float> air_density;   // atmosphere -> aerodynamics dependency
    std::vector<float> lift;
    std::vector<float> drag;
    std::vector<float> thrust;
    std::vector<float> mass;

    void resize(std::size_t count);
    [[nodiscard]] std::size_t size() const noexcept { return position.size(); }
};

// One complete simulation state.
struct WorldState {
    VehicleArrays  vehicles;
    NpcArrays      npcs;
    AircraftArrays aircraft;

    Vec3 player_position{};

    void resize(std::size_t vehicle_count, std::size_t npc_count, std::size_t aircraft_count);
};

// The world extent, in metres. Entities wrap at the edges, which keeps the
// simulation bounded without needing collision against a boundary.
inline constexpr float kWorldSize = 5000.0f;

// Fixed simulation timestep (S68). Physics must never be tied to a variable frame
// rate - besides being wrong, it would make a benchmark unreproducible.
inline constexpr float kFixedTimestep = 1.0f / 60.0f;

// Double-buffered pair. `current` is read-only during a tick; systems write
// `next`, and the two are swapped at the tick boundary.
class World {
public:
    World(std::size_t vehicle_count, std::size_t npc_count, std::size_t aircraft_count,
          std::uint64_t seed);

    [[nodiscard]] const WorldState& current() const noexcept { return *current_; }
    [[nodiscard]] WorldState&       next() noexcept { return *next_; }

    void swap_buffers() noexcept;

    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }
    [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }
    void                        advance_tick() noexcept { ++tick_; }

private:
    void seed_initial_state();

    WorldState  buffers_[2];
    WorldState* current_ = &buffers_[0];
    WorldState* next_    = &buffers_[1];

    std::uint64_t seed_ = 0;
    std::uint64_t tick_ = 0;
};

} // namespace tbworld

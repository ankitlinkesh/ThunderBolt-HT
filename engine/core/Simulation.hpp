// Thunderbolt Open World - the headless simulation.
//
// This is the flagship benchmark workload (S22, S52). No window, no renderer, no
// audio, no wall-clock coupling: a fixed 60 Hz timestep and a seeded start, so a
// run is reproducible to the bit.
//
// It is headless for a measurement reason, not a convenience one. With a renderer
// in the loop on an integrated GPU every scene is GPU-bound, both runtimes report
// the same frame time, and the A/B comparison measures nothing. Rendering, when it
// exists, is visualisation and sits outside the measured path.
#pragma once

#include "World.hpp"

#include <thunderbolt/api/ITaskRuntime.hpp>

#include <cstddef>
#include <cstdint>

namespace tbworld {

struct SceneSpec {
    const char*   name           = "custom";
    std::size_t   vehicles       = 0;
    std::size_t   npcs           = 0;
    std::size_t   aircraft       = 0;
};

// The named scenes from S56's game_benchmarks list.
[[nodiscard]] const SceneSpec* find_scene(const char* name) noexcept;
void                           list_scenes();

struct FrameStats {
    std::uint64_t tasks_submitted = 0;
    std::uint64_t stages          = 0;
};

class Simulation {
public:
    Simulation(SceneSpec scene, std::uint64_t seed);

    // Advances one fixed timestep, expressed as a task graph on `runtime`.
    // Blocks until the tick is complete, then swaps buffers.
    void tick(thunderbolt::ITaskRuntime& runtime);

    // Runs the whole tick on the calling thread with no runtime involved. The
    // serial reference for the workload-weight gate: if T1 is not comfortably
    // above per-frame scheduler cost, the scene is too cheap to say anything
    // about scheduling and its speedup must not be reported.
    void tick_serial();

    [[nodiscard]] std::uint64_t state_hash() const;

    [[nodiscard]] const World& world() const noexcept { return world_; }
    [[nodiscard]] const SceneSpec& scene() const noexcept { return scene_; }
    [[nodiscard]] const FrameStats& last_frame() const noexcept { return last_frame_; }

private:
    SceneSpec  scene_;
    World      world_;
    FrameStats last_frame_;
};

} // namespace tbworld

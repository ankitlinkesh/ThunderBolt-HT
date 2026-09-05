// Thunderbolt Open World - bit-exact world state hash.
//
// THIS IS THE CORRECTNESS PROOF. ThreadSanitizer is unavailable on this
// toolchain, so the substitute is a much stronger claim than "the tests pass":
// the same seed must produce a bit-identical hash under both runtimes and at
// every worker count from 1 to 8. A scheduler race that corrupts one float in one
// tick out of thousands changes the hash; a stress test that only checks task
// counts would not notice.
//
// It hashes RAW BITS, not values. Comparing with a tolerance would defeat the
// purpose: the point is to detect a low-bit divergence, which is exactly what a
// race or a worker-count-dependent reduction produces first.
//
// The walk is strictly in index order and single-threaded. Hashing is cheap
// relative to a tick, and making the proof itself depend on parallel scheduling
// would be circular.
#pragma once

#include "World.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tbworld {

class StateHasher {
public:
    // FNV-1a, 64-bit. Chosen for being trivial to reimplement in an analysis
    // script - a hash nobody else can compute is not much use for comparing runs.
    void feed_bytes(const void* data, std::size_t size) noexcept {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            value_ ^= bytes[i];
            value_ *= 0x100000001B3ULL;
        }
    }

    void feed(float v) noexcept {
        // Through memcpy rather than a reinterpret_cast: type punning through a
        // pointer cast is undefined, and this compiles to the same load.
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        feed_bytes(&bits, sizeof(bits));
    }

    void feed(Vec3 v) noexcept {
        feed(v.x);
        feed(v.y);
        feed(v.z);
    }

    void feed(std::uint32_t v) noexcept { feed_bytes(&v, sizeof(v)); }
    void feed(std::uint64_t v) noexcept { feed_bytes(&v, sizeof(v)); }
    void feed(SimLod v) noexcept { feed(static_cast<std::uint32_t>(v)); }

    template <typename T>
    void feed_array(const std::vector<T>& values) noexcept {
        for (const T& value : values) {
            feed(value);
        }
    }

    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

private:
    std::uint64_t value_ = 0xCBF29CE484222325ULL;
};

// Hashes every field that a system may write. A field left out here is a field
// whose corruption the harness cannot see, so this list must stay exhaustive.
[[nodiscard]] inline std::uint64_t hash_state(const WorldState& state) noexcept {
    StateHasher hasher;

    hasher.feed_array(state.vehicles.position);
    hasher.feed_array(state.vehicles.velocity);
    hasher.feed_array(state.vehicles.heading);
    hasher.feed_array(state.vehicles.steer);
    hasher.feed_array(state.vehicles.throttle);
    hasher.feed_array(state.vehicles.engine_rpm);
    hasher.feed_array(state.vehicles.wheel_load);
    hasher.feed_array(state.vehicles.mass);
    hasher.feed_array(state.vehicles.lod);
    hasher.feed_array(state.vehicles.lane);

    hasher.feed_array(state.npcs.position);
    hasher.feed_array(state.npcs.velocity);
    hasher.feed_array(state.npcs.target);
    hasher.feed_array(state.npcs.speed);
    hasher.feed_array(state.npcs.behaviour);
    hasher.feed_array(state.npcs.lod);
    hasher.feed_array(state.npcs.nearest_distance);
    hasher.feed_array(state.npcs.nearest_index);

    hasher.feed_array(state.aircraft.position);
    hasher.feed_array(state.aircraft.velocity);
    hasher.feed_array(state.aircraft.heading);
    hasher.feed_array(state.aircraft.pitch);
    hasher.feed_array(state.aircraft.throttle);
    hasher.feed_array(state.aircraft.altitude);
    hasher.feed_array(state.aircraft.air_density);
    hasher.feed_array(state.aircraft.lift);
    hasher.feed_array(state.aircraft.drag);
    hasher.feed_array(state.aircraft.thrust);
    hasher.feed_array(state.aircraft.mass);

    hasher.feed(state.player_position);
    return hasher.value();
}

[[nodiscard]] inline std::string hash_to_string(std::uint64_t hash) {
    static const char* digits = "0123456789abcdef";
    std::string        out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = digits[hash & 0xF];
        hash >>= 4;
    }
    return out;
}

} // namespace tbworld

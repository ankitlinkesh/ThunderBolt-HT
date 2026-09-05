// Thunderbolt Open World - simulation math.
//
// DETERMINISM IS THE CONTRACT HERE, not performance. The state hash must be
// bit-identical across runtimes and across worker counts, and that only holds if
// every arithmetic result is a function of the data alone - never of how the work
// was divided.
//
// What that forbids, concretely:
//   - No reduction whose partitioning depends on worker count. FP addition is not
//     associative, so summing the same values in a different order gives a
//     different answer in the low bits.
//   - No atomic float accumulation, and no completion-order folds.
//   - No fast-math and no FMA contraction. Both are pinned in the compiler flags;
//     see thunderbolt/cmake/ThunderboltCompilerFlags.cmake.
//
// What it permits: per-entity arithmetic, which is the overwhelming majority of a
// simulation frame. Each entity's new state depends only on its own previous
// state and on reads of a previous-frame snapshot, so the result is independent
// of scheduling by construction rather than by care.
#pragma once

#include <cmath>
#include <cstdint>

namespace tbworld {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

[[nodiscard]] inline Vec3 add(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] inline Vec3 sub(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] inline Vec3 scale(Vec3 v, float s) noexcept {
    return Vec3{v.x * s, v.y * s, v.z * s};
}

[[nodiscard]] inline float dot(Vec3 a, Vec3 b) noexcept {
    // Written as an explicit left-to-right sum. The order is fixed in the source,
    // so it cannot vary between builds, and contraction is disabled so the
    // compiler cannot fuse the multiply-adds into a differently-rounded form.
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

[[nodiscard]] inline float length_squared(Vec3 v) noexcept { return dot(v, v); }

[[nodiscard]] inline float length(Vec3 v) noexcept { return std::sqrt(length_squared(v)); }

[[nodiscard]] inline Vec3 normalized_or_zero(Vec3 v) noexcept {
    const float len = length(v);
    if (len <= 1e-6f) {
        return Vec3{};
    }
    const float inverse = 1.0f / len;
    return scale(v, inverse);
}

[[nodiscard]] inline float clampf(float value, float low, float high) noexcept {
    return (value < low) ? low : ((value > high) ? high : value);
}

// Deterministic wrap into [0, limit). std::fmod is exact for these magnitudes and
// its result does not vary with optimisation level.
[[nodiscard]] inline float wrapf(float value, float limit) noexcept {
    float wrapped = std::fmod(value, limit);
    if (wrapped < 0.0f) {
        wrapped += limit;
    }
    return wrapped;
}

// splitmix64. Deterministic, stateless given a seed, and - importantly - a
// function of the ENTITY INDEX rather than of a shared stream. A shared RNG
// consumed by parallel workers would hand out different values depending on who
// drew first, which is exactly the scheduling dependence the hash must not have.
[[nodiscard]] inline std::uint64_t mix64(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// Uniform float in [0, 1) derived from a seed and an index. Callers pass the
// entity index, so every entity's stream is independent and reproducible no
// matter which worker evaluates it.
[[nodiscard]] inline float random_unit(std::uint64_t seed, std::uint64_t index,
                                       std::uint64_t stream = 0) noexcept {
    const std::uint64_t bits = mix64(seed ^ mix64(index ^ (stream * 0x2545F4914F6CDD1DULL)));
    // 24 bits gives an exactly-representable float; using more would round and
    // reintroduce a dependence on rounding mode.
    return static_cast<float>(bits >> 40) * (1.0f / 16777216.0f);
}

[[nodiscard]] inline float random_range(std::uint64_t seed, std::uint64_t index, float low,
                                        float high, std::uint64_t stream = 0) noexcept {
    return low + (high - low) * random_unit(seed, index, stream);
}

} // namespace tbworld

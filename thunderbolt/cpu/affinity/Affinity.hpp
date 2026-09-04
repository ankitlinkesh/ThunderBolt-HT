// Thunderbolt HT - optional worker affinity.
//
// S17 is explicit that affinity must NOT be forced on by default: "The OS
// scheduler may outperform naive manual pinning. Benchmark both." That is not
// hedging - on a 15 W part the OS migrates threads partly to spread heat, and
// pinning can cost more in throttling than it gains in cache locality.
//
// So this is a knob that defaults to off, lives in RuntimeConfig (shared by every
// runtime, so it stays constant across an A/B rather than being one runtime's
// private advantage), and reports whether it actually took effect - a silently
// ignored pinning request would make a benchmark row claim a configuration that
// never ran.
#pragma once

#include <thunderbolt/api/RuntimeConfig.hpp>

#include <cstdint>

namespace thunderbolt {

// Pins the calling thread according to `mode`. Returns false when the request
// could not be honoured (mode disabled, topology unknown, or the OS refused), so
// callers can record what actually happened rather than what was asked for.
bool apply_worker_affinity(std::uint32_t worker_index, AffinityMode mode);

} // namespace thunderbolt

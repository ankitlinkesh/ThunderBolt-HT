// Runs ONE behavioural suite against EVERY runtime.
//
// This file is the structural guarantee behind the A/B methodology: if the two
// runtimes ever disagree about what the task API means, the build fails here
// rather than the disagreement being measured later and reported as a scheduling
// result.
#include "RuntimeConformance.hpp"

#include <thunderbolt/runtime/StandardRuntime.hpp>
#include <thunderbolt/runtime/ThunderboltRuntime.hpp>

TB_RUNTIME_CONFORMANCE_SUITE(thunderbolt::StandardRuntime, "[standard]")
TB_RUNTIME_CONFORMANCE_SUITE(thunderbolt::ThunderboltRuntime, "[thunderbolt]")


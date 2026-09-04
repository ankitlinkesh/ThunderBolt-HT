// Runs the dependency suite against EVERY runtime.
//
// Dependency resolution lives in RuntimeBase so the two runtimes cannot diverge;
// this is what would catch it if that ever stopped being true.
#include "DependencyConformance.hpp"

#include <thunderbolt/runtime/StandardRuntime.hpp>
#include <thunderbolt/runtime/ThunderboltRuntime.hpp>

TB_DEPENDENCY_CONFORMANCE_SUITE(thunderbolt::StandardRuntime, "[standard]")
TB_DEPENDENCY_CONFORMANCE_SUITE(thunderbolt::ThunderboltRuntime, "[thunderbolt]")

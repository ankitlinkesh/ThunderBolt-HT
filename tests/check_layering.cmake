# Architectural layering guard (S4, S66).
#
# Run via `ctest`. Fails the build if the one-way dependency arrow is violated.
# A grep test, not a build test: it catches the violation at the point someone
# writes the include, not later when the standalone build mysteriously breaks.

if(NOT DEFINED REPO_ROOT)
    message(FATAL_ERROR "REPO_ROOT not set")
endif()

set(violations "")

# --- Rule 1: nothing under thunderbolt/ may include engine/ or game/ headers ---
file(GLOB_RECURSE runtime_sources
     "${REPO_ROOT}/thunderbolt/*.hpp"
     "${REPO_ROOT}/thunderbolt/*.cpp"
     "${REPO_ROOT}/thunderbolt/*.h")

foreach(src IN LISTS runtime_sources)
    file(STRINGS "${src}" bad_lines REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](engine|game)/")
    foreach(line IN LISTS bad_lines)
        list(APPEND violations "${src}: runtime includes application code -> ${line}")
    endforeach()
endforeach()

# --- Rule 2: engine/ and game/ may not reach past the ITaskRuntime abstraction ---
# S66 names these explicitly. Tooling and Thunderbolt-specific profiling are
# exempt by living under tools/, which is not scanned.
set(internal_symbols "ThunderboltWorker|ThunderboltDeque|ThunderboltInternalScheduler")

file(GLOB_RECURSE app_sources
     "${REPO_ROOT}/engine/*.hpp" "${REPO_ROOT}/engine/*.cpp"
     "${REPO_ROOT}/game/*.hpp"   "${REPO_ROOT}/game/*.cpp")

foreach(src IN LISTS app_sources)
    file(STRINGS "${src}" bad_lines REGEX "${internal_symbols}")
    foreach(line IN LISTS bad_lines)
        list(APPEND violations "${src}: application reaches into runtime internals -> ${line}")
    endforeach()
endforeach()

if(violations)
    list(JOIN violations "\n  " report)
    message(FATAL_ERROR "Architectural layering violated:\n  ${report}")
endif()

message(STATUS "Layering OK: runtime is free of application dependencies")

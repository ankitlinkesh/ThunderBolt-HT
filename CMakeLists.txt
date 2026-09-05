# Thunderbolt HT + Thunderbolt Open World.
#
# Two systems in one repository, deliberately separable:
#   thunderbolt/  the parallel execution runtime  (must build with engine/ absent)
#   engine/,game/ the simulation that exercises it
#
# The dependency arrow points one way only. engine/ and game/ may depend on
# thunderbolt/; nothing under thunderbolt/ may depend on them (S4, S66).

cmake_minimum_required(VERSION 3.25)

project(ThunderboltHT
    VERSION 0.1.0
    DESCRIPTION "A parallel CPU execution runtime, and the simulation that benchmarks it"
    LANGUAGES CXX
)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    # Single output location keeps benchmark invocation paths stable across
    # generators, which matters because S87 records the binary that produced a run.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    include(CTest)
endif()

# The runtime. Self-contained; see thunderbolt/CMakeLists.txt.
add_subdirectory(thunderbolt)

# The simulation that exercises the runtime. One-way dependency: engine/ may use
# thunderbolt/, never the reverse.
add_subdirectory(engine)
add_subdirectory(game_benchmarks)

# Repo-level tests, including the guard that thunderbolt/ has not grown a
# dependency on engine/ or game/.
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
    add_subdirectory(tests)
endif()

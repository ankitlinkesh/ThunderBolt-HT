// thunderbolt-sim - the headless deterministic simulation driver.
//
// Two jobs, and the first is the more important:
//
//   --hash   Run N ticks and print the world-state hash. The SAME seed must
//            produce the SAME hash under either runtime and at every worker
//            count. That is the correctness proof this project uses in place of
//            ThreadSanitizer, which is unavailable on this toolchain. A scheduler
//            race that corrupts one float in one tick changes the hash.
//
//   default  Time the tick loop, so the same workload can be compared across
//            runtimes with everything else held constant (S94).
#include <engine/core/Simulation.hpp>
#include <engine/core/StateHash.hpp>

#include <thunderbolt/api/BuildInfo.hpp>
#include <thunderbolt/cpu/topology/CpuTopology.hpp>
#include <thunderbolt/runtime/RuntimeBase.hpp>
#include <thunderbolt/runtime/StandardRuntime.hpp>
#include <thunderbolt/runtime/ThunderboltRuntime.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

void print_usage() {
    std::printf(
        "thunderbolt-sim - headless deterministic simulation\n"
        "\n"
        "usage: thunderbolt-sim --scene <name> [options]\n"
        "\n"
        "options:\n"
        "  --scene NAME     Scene to run (--list-scenes to see them)\n"
        "  --seed N         RNG seed (default 42)\n"
        "  --ticks N        Fixed timesteps to simulate (default 600 = 10s at 60Hz)\n"
        "  --runtime NAME   standard | thunderbolt | serial (default thunderbolt)\n"
        "  --workers N      Worker threads (default: one per logical processor)\n"
        "  --hash           Print the state hash and exit; implies a quiet run\n"
        "  --list-scenes    List the available scenes\n"
        "\n"
        "The hash is the correctness proof. It must be identical for a given seed\n"
        "across runtimes and across worker counts:\n"
        "\n"
        "  thunderbolt-sim --scene full_mixed --seed 42 --runtime serial       --hash\n"
        "  thunderbolt-sim --scene full_mixed --seed 42 --runtime standard -w 1 --hash\n"
        "  thunderbolt-sim --scene full_mixed --seed 42 --runtime thunderbolt -w 8 --hash\n");
}

bool parse_uint(const char* text, unsigned long long& out) {
    char*                    end   = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    using namespace thunderbolt;
    using namespace tbworld;

    std::string   scene_name = "full_mixed";
    std::string   runtime_name = "thunderbolt";
    std::uint64_t seed    = 42;
    std::uint64_t ticks   = 600;
    std::uint32_t workers = 0;
    bool          hash_only = false;

    for (int i = 1; i < argc; ++i) {
        const char* arg  = argv[i];
        const bool  more = (i + 1) < argc;

        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            print_usage();
            return 0;
        }
        if (std::strcmp(arg, "--list-scenes") == 0) {
            list_scenes();
            return 0;
        }
        if (std::strcmp(arg, "--hash") == 0) {
            hash_only = true;
        } else if (std::strcmp(arg, "--scene") == 0 && more) {
            scene_name = argv[++i];
        } else if (std::strcmp(arg, "--runtime") == 0 && more) {
            runtime_name = argv[++i];
        } else if ((std::strcmp(arg, "--workers") == 0 || std::strcmp(arg, "-w") == 0) && more) {
            unsigned long long value = 0;
            if (!parse_uint(argv[++i], value)) {
                std::fprintf(stderr, "error: --workers needs a number\n");
                return 2;
            }
            workers = static_cast<std::uint32_t>(value);
        } else if (std::strcmp(arg, "--seed") == 0 && more) {
            if (!parse_uint(argv[++i], seed)) {
                std::fprintf(stderr, "error: --seed needs a number\n");
                return 2;
            }
        } else if (std::strcmp(arg, "--ticks") == 0 && more) {
            if (!parse_uint(argv[++i], ticks)) {
                std::fprintf(stderr, "error: --ticks needs a number\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n\n", arg);
            print_usage();
            return 2;
        }
    }

    const SceneSpec* scene = find_scene(scene_name.c_str());
    if (scene == nullptr) {
        std::fprintf(stderr, "error: unknown scene '%s'\n\n", scene_name.c_str());
        list_scenes();
        return 2;
    }

    Simulation simulation(*scene, seed);

    RuntimeConfig config;
    config.worker_count  = workers;
    config.task_capacity = 262144;

    std::unique_ptr<ITaskRuntime> runtime;
    const bool                    serial = (runtime_name == "serial");
    if (!serial) {
        if (runtime_name == "standard") {
            runtime = std::make_unique<StandardRuntime>(config);
        } else if (runtime_name == "thunderbolt") {
            runtime = std::make_unique<ThunderboltRuntime>(config);
        } else {
            std::fprintf(stderr, "error: --runtime must be standard, thunderbolt or serial\n");
            return 2;
        }
    }

    if (!hash_only) {
        const auto& build = build_info();
        std::printf("Thunderbolt Open World  [%.*s, %.*s]\n",
                    static_cast<int>(build.compiler.size()), build.compiler.data(),
                    static_cast<int>(build.configuration.size()), build.configuration.data());
        std::printf("scene=%s  vehicles=%zu npcs=%zu aircraft=%zu  seed=%llu ticks=%llu\n",
                    scene->name, scene->vehicles, scene->npcs, scene->aircraft,
                    static_cast<unsigned long long>(seed),
                    static_cast<unsigned long long>(ticks));
        std::printf("runtime=%s workers=%u\n\n", runtime_name.c_str(),
                    serial ? 1u : runtime->worker_count());
    }

    // Watchdog. The simulation is the first workload to exercise many-dependency
    // barriers submitted from an external thread, and it exposed an intermittent
    // hang there. A plain timeout says only "it stopped"; this says which tick and
    // how much work was still outstanding, which is the difference between
    // guessing and diagnosing.
    std::atomic<std::uint64_t> current_tick{0};
    std::atomic<bool>          finished{false};
    std::thread watchdog([&current_tick, &finished, &runtime, serial] {
        for (int elapsed = 0; elapsed < 200 && !finished.load(std::memory_order_acquire);
             ++elapsed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!finished.load(std::memory_order_acquire)) {
            std::fprintf(stderr, "\nWATCHDOG: stalled at tick %llu",
                         static_cast<unsigned long long>(current_tick.load()));
            if (!serial) {
                std::fprintf(stderr, ", outstanding=%llu completed=%llu",
                             static_cast<unsigned long long>(
                                 static_cast<thunderbolt::RuntimeBase*>(runtime.get())
                                     ->outstanding_task_count()),
                             static_cast<unsigned long long>(
                                 static_cast<thunderbolt::RuntimeBase*>(runtime.get())
                                     ->completed_task_count()));
            }
            std::fprintf(stderr, "\n");
            if (!serial) {
                static_cast<thunderbolt::RuntimeBase*>(runtime.get())->dump_outstanding();
            }
            std::fflush(stderr);
            std::_Exit(3);
        }
    });

    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t t = 0; t < ticks; ++t) {
        current_tick.store(t, std::memory_order_release);
        if (serial) {
            simulation.tick_serial();
        } else {
            simulation.tick(*runtime);
        }
    }
    const auto finish = std::chrono::steady_clock::now();
    finished.store(true, std::memory_order_release);
    watchdog.join();

    const double seconds = std::chrono::duration<double>(finish - start).count();
    const std::uint64_t hash = simulation.state_hash();

    if (hash_only) {
        std::printf("%s\n", hash_to_string(hash).c_str());
        return 0;
    }

    const double per_tick_ms = (ticks > 0) ? (seconds * 1000.0 / static_cast<double>(ticks)) : 0.0;

    std::printf("state hash      : %s\n", hash_to_string(hash).c_str());
    std::printf("total time      : %.4f s\n", seconds);
    std::printf("per tick        : %.4f ms\n", per_tick_ms);
    std::printf("tasks per tick  : %llu across %llu stages\n",
                static_cast<unsigned long long>(simulation.last_frame().tasks_submitted),
                static_cast<unsigned long long>(simulation.last_frame().stages));

    // A fixed timestep is 16.67 ms of simulated time. Reporting the ratio makes
    // "would this keep up in real time?" answerable without implying a frame rate
    // the renderer does not exist to deliver.
    std::printf("real-time budget: %.1f%% of a 60 Hz tick\n", per_tick_ms / 16.6667 * 100.0);
    return 0;
}

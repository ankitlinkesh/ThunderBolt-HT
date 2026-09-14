#include <thunderbolt/api/ITaskRuntime.hpp>
#include <thunderbolt/api/RuntimeConfig.hpp>
#include <thunderbolt/runtime/ThunderboltRuntime.hpp>

#include <atomic>
#include <cstdio>

int main() {
    thunderbolt::RuntimeConfig config;
    config.worker_count = 4;
    thunderbolt::ThunderboltRuntime runtime(config);

    std::atomic<int> ran{0};
    for (int i = 0; i < 1000; ++i) {
        (void)runtime.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    }
    runtime.wait_all();

    std::printf("thunderbolt Conan package works: ran = %d (expected 1000)\n", ran.load());
    return ran.load() == 1000 ? 0 : 1;
}

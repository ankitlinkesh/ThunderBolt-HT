#include "TestHarness.hpp"

namespace thunderbolt::test {

int run_all() {
    int passed = 0;
    int failed = 0;

    for (const TestCase& tc : registry()) {
        try {
            tc.fn();
            ++passed;
            std::printf("[ PASS ] %.*s\n", static_cast<int>(tc.name.size()), tc.name.data());
        } catch (const CheckFailure& f) {
            ++failed;
            std::printf("[ FAIL ] %.*s\n         %s\n",
                        static_cast<int>(tc.name.size()), tc.name.data(), f.message.c_str());
        } catch (const std::exception& e) {
            ++failed;
            std::printf("[ FAIL ] %.*s\n         unexpected exception: %s\n",
                        static_cast<int>(tc.name.size()), tc.name.data(), e.what());
        }
    }

    std::printf("\n%d passed, %d failed, %d total\n",
                passed, failed, static_cast<int>(registry().size()));
    return failed == 0 ? 0 : 1;
}

} // namespace thunderbolt::test

int main() { return thunderbolt::test::run_all(); }

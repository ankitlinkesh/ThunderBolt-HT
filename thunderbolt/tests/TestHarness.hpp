// Minimal self-registering test harness.
//
// Deliberately dependency-free: pulling in a framework would put third-party
// headers under /W4 /WX and would weaken the "thunderbolt/ builds standalone"
// guarantee (S4). If the suite outgrows this, revisit - but SYSTEM-include the
// framework when doing so.
#pragma once

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace thunderbolt::test {

using TestFn = void (*)();

struct TestCase {
    std::string_view name;
    std::string_view file;
    int              line;
    TestFn           fn;
};

// Registry lives in a function-local static: avoids static init order issues
// between translation units registering at namespace scope.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(std::string_view name, std::string_view file, int line, TestFn fn) {
        registry().push_back(TestCase{name, file, line, fn});
    }
};

// Thrown by a failed check so the runner can report and continue to the next case.
struct CheckFailure {
    std::string message;
};

[[noreturn]] inline void fail(std::string_view expr, std::string_view file, int line,
                              std::string_view detail = {}) {
    std::string msg;
    msg += file;
    msg += ":";
    msg += std::to_string(line);
    msg += ": check failed: ";
    msg += expr;
    if (!detail.empty()) {
        msg += "  (";
        msg += detail;
        msg += ")";
    }
    throw CheckFailure{msg};
}

int run_all();

} // namespace thunderbolt::test

#define TB_TEST_CAT_(a, b) a##b
#define TB_TEST_CAT(a, b)  TB_TEST_CAT_(a, b)

// Defines and registers a test case.
//
// Identifiers are keyed on __COUNTER__, not __LINE__. A macro that expands to
// several TB_TEST cases - as the runtime conformance suite does, registering one
// suite per runtime - puts them all on the SAME source line, so __LINE__ would
// collide and only the first case would compile. __COUNTER__ is unique per use.
#define TB_TEST(name) TB_TEST_IMPL(name, __COUNTER__)

#define TB_TEST_IMPL(name, id)                                                     \
    static void TB_TEST_CAT(tb_test_fn_, id)();                                    \
    static const ::thunderbolt::test::Registrar TB_TEST_CAT(tb_test_reg_, id)(     \
        name, __FILE__, __LINE__, &TB_TEST_CAT(tb_test_fn_, id));                  \
    static void TB_TEST_CAT(tb_test_fn_, id)()

#define TB_CHECK(expr)                                                             \
    do {                                                                           \
        if (!(expr)) {                                                             \
            ::thunderbolt::test::fail(#expr, __FILE__, __LINE__);                  \
        }                                                                          \
    } while (false)

#define TB_CHECK_EQ(a, b)                                                          \
    do {                                                                           \
        if (!((a) == (b))) {                                                       \
            ::thunderbolt::test::fail(#a " == " #b, __FILE__, __LINE__);           \
        }                                                                          \
    } while (false)

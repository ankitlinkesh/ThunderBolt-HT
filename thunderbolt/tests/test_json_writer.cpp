// The benchmark results writer.
//
// Worth testing precisely because its failures are silent. Two bugs shipped here
// before this test existed, and neither made anything crash:
//
//   1. Every object opened with a stray leading comma, because the container
//      state was initialised as if an entry had already been written.
//   2. Every STRING field wrote `true`. A string literal is a const char*, and
//      const char* -> bool is a standard conversion while const char* ->
//      string_view is a user-defined one, so the bool overload won and the value
//      was silently replaced.
//
// Both produced files that looked plausible at a glance and were unparseable.
// The assertions below compare exact output, because "it looked fine" is what let
// them through.
#include "TestHarness.hpp"

#include "../benchmarks/harness/Json.hpp"

#include <sstream>
#include <string>

using thunderbolt::bench::JsonWriter;

namespace {

std::string render(void (*build)(JsonWriter&)) {
    std::ostringstream out;
    JsonWriter         json(out);
    build(json);
    return out.str();
}

} // namespace

TB_TEST("json: an empty object emits no stray comma") {
    const std::string text = render([](JsonWriter& json) {
        json.begin_object();
        json.end_object();
    });
    TB_CHECK(text == "{}");
}

TB_TEST("json: a single field has no leading comma") {
    const std::string text = render([](JsonWriter& json) {
        json.begin_object();
        json.field("a", std::uint64_t{1});
        json.end_object();
    });
    TB_CHECK(text == "{\n  \"a\": 1\n}");
}

TB_TEST("json: string literals are written as strings, not booleans") {
    // The exact bug that made every results file useless.
    const std::string text = render([](JsonWriter& json) {
        json.begin_object();
        json.field("name", "granularity");
        json.end_object();
    });
    TB_CHECK(text.find("\"granularity\"") != std::string::npos);
    TB_CHECK(text.find("true") == std::string::npos);
}

TB_TEST("json: booleans are still written as booleans") {
    const std::string text = render([](JsonWriter& json) {
        json.begin_object();
        json.field("flag", true);
        json.field("other", false);
        json.end_object();
    });
    TB_CHECK(text.find("\"flag\": true") != std::string::npos);
    TB_CHECK(text.find("\"other\": false") != std::string::npos);
}

TB_TEST("json: fields are comma-separated exactly once") {
    const std::string text = render([](JsonWriter& json) {
        json.begin_object();
        json.field("a", std::uint64_t{1});
        json.field("b", std::uint64_t{2});
        json.end_object();
    });
    std::size_t commas = 0;
    for (char c : text) {
        commas += (c == ',') ? 1u : 0u;
    }
    TB_CHECK_EQ(commas, 1u);
}

TB_TEST("json: nested objects and arrays are balanced") {
    const std::string text = render([](JsonWriter& json) {
        json.begin_object();
        json.begin_object("inner");
        json.field("x", std::uint64_t{1});
        json.end_object();
        json.begin_array("items");
        json.begin_object();
        json.field("y", std::uint64_t{2});
        json.end_object();
        json.end_array();
        json.end_object();
    });

    std::size_t braces = 0, brackets = 0;
    for (char c : text) {
        braces += (c == '{') ? 1u : 0u;
        braces -= (c == '}') ? 1u : 0u;
        brackets += (c == '[') ? 1u : 0u;
        brackets -= (c == ']') ? 1u : 0u;
    }
    TB_CHECK_EQ(braces, 0u);
    TB_CHECK_EQ(brackets, 0u);
    TB_CHECK(text.find("{,") == std::string::npos);  // the leading-comma bug
    TB_CHECK(text.find("[,") == std::string::npos);
}

TB_TEST("json: strings with quotes and newlines are escaped") {
    const std::string text = render([](JsonWriter& json) {
        json.begin_object();
        json.field("s", "a\"b\nc");
        json.end_object();
    });
    // Expected: a, backslash, quote, b, backslash, n, c.
    TB_CHECK(text.find("a\\\"b\\nc") != std::string::npos);
}

TB_TEST("json: doubles keep round-trip precision") {
    // A result that loses digits on the way to disk cannot be re-analysed, which
    // defeats the point of writing it out.
    const std::string text = render([](JsonWriter& json) {
        json.begin_object();
        json.field("v", 0.1234567890123456789);
        json.end_object();
    });
    TB_CHECK(text.find("0.123456789012345") != std::string::npos);
}

// Thunderbolt HT - minimal JSON writer for benchmark results.
//
// S87 requires results in a machine-readable format. Hand-rolled rather than
// pulled in: the output is a few nested objects and arrays, and a dependency here
// would have to be SYSTEM-included to survive /W4 /WX for no real benefit.
//
// Numbers are written with full round-trip precision. A benchmark result that
// loses digits on the way to disk cannot be re-analysed later, which defeats the
// point of writing it out at all.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace thunderbolt::bench {

class JsonWriter {
public:
    explicit JsonWriter(std::ostream& out) : out_(out) {}

    void begin_object() {
        separate();
        out_ << "{";
        stack_.push_back(State{false, false});
    }

    void begin_object(std::string_view key) {
        write_key(key);
        out_ << "{";
        stack_.push_back(State{false, false});
    }

    void end_object() {
        const bool had_entries = !stack_.empty() && stack_.back().any_written;
        stack_.pop_back();
        if (had_entries) {
            newline_indent();
        }
        out_ << "}";
    }

    void begin_array(std::string_view key) {
        write_key(key);
        out_ << "[";
        stack_.push_back(State{false, true});
    }

    void end_array() {
        const bool had_entries = !stack_.empty() && stack_.back().any_written;
        stack_.pop_back();
        if (had_entries) {
            newline_indent();
        }
        out_ << "]";
    }

    void field(std::string_view key, std::string_view value) {
        write_key(key);
        write_string(value);
    }

    void field(std::string_view key, const std::string& value) {
        field(key, std::string_view{value});
    }

    // MUST exist, and must come before the bool overload is reachable.
    //
    // A string literal is a const char*, and const char* -> bool is a STANDARD
    // conversion while const char* -> string_view is a user-defined one. Standard
    // wins, so without this overload every field("k", "some text") silently wrote
    // `true`. It did exactly that, and produced results files whose every string
    // value was the literal true, until a parser rejected them.
    void field(std::string_view key, const char* value) {
        write_key(key);
        write_string(value != nullptr ? std::string_view{value} : std::string_view{});
    }

    void field(std::string_view key, bool value) {
        write_key(key);
        out_ << (value ? "true" : "false");
    }

    void field(std::string_view key, std::uint64_t value) {
        write_key(key);
        out_ << value;
    }

    void field(std::string_view key, std::int64_t value) {
        write_key(key);
        out_ << value;
    }

    void field(std::string_view key, std::uint32_t value) {
        field(key, static_cast<std::uint64_t>(value));
    }

    void field(std::string_view key, int value) { field(key, static_cast<std::int64_t>(value)); }

    void field(std::string_view key, double value) {
        write_key(key);
        write_double(value);
    }

    // Array elements.
    void value(double v) {
        separate();
        write_double(v);
    }
    void value(std::uint64_t v) {
        separate();
        out_ << v;
    }

private:
    struct State {
        bool any_written;  // false until the first entry of this container is emitted
        bool is_array;
    };

    void write_double(double v) {
        // 17 significant digits round-trips an IEEE double exactly.
        const auto previous = out_.precision(17);
        out_ << v;
        out_.precision(previous);
    }

    void write_string(std::string_view value) {
        out_ << '"';
        for (char c : value) {
            switch (c) {
            case '"': out_ << "\\\""; break;
            case '\\': out_ << "\\\\"; break;
            case '\n': out_ << "\\n"; break;
            case '\r': out_ << "\\r"; break;
            case '\t': out_ << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out_ << "\\u00" << "0123456789abcdef"[(c >> 4) & 0xF]
                         << "0123456789abcdef"[c & 0xF];
                } else {
                    out_ << c;
                }
            }
        }
        out_ << '"';
    }

    void separate() {
        if (stack_.empty()) {
            return;
        }
        if (stack_.back().any_written) {
            out_ << ",";
        }
        stack_.back().any_written = true;
        newline_indent();
    }

    void write_key(std::string_view key) {
        separate();
        write_string(key);
        out_ << ": ";
    }

    void newline_indent() {
        out_ << "\n";
        for (std::size_t i = 0; i < stack_.size(); ++i) {
            out_ << "  ";
        }
    }

    std::ostream&      out_;
    std::vector<State> stack_;
};

} // namespace thunderbolt::bench

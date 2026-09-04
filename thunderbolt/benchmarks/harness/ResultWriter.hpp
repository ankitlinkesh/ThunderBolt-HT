// Thunderbolt HT - benchmark result serialisation.
//
// Every number this project publishes must be traceable to a row here (S55, S88).
// The environment block is written for EVERY result, not once per file, so a row
// copied out of context still carries the machine and build that produced it.
#pragma once

#include "BenchmarkRunner.hpp"
#include "Environment.hpp"
#include "Json.hpp"

#include <string_view>

namespace thunderbolt::bench {

void write_environment(JsonWriter& json, const Environment& environment);
void write_options(JsonWriter& json, const RunOptions& options);
void write_summary(JsonWriter& json, std::string_view key, const Summary& summary);
void write_leg(JsonWriter& json, const LegResult& leg, bool include_samples);

// Writes the shared header of a results document: schema, environment, options.
// Leaves the top-level object OPEN for the caller to add its own fields.
void begin_result_document(JsonWriter& json, std::string_view benchmark_name,
                           const Environment& environment, const RunReport& report);

} // namespace thunderbolt::bench

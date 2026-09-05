#include "ResultWriter.hpp"

namespace thunderbolt::bench {

void write_environment(JsonWriter& json, const Environment& environment) {
    json.begin_object("environment");
    json.field("cpu", environment.cpu_brand);

    // Written as a signed value with -1 meaning "not detected", so a consumer
    // cannot mistake an undetected core count for a single-core machine. S16
    // exists because logical processors are not cores; refusing to guess is the
    // point.
    if (environment.physical_cores > 0) {
        json.field("physical_cores", static_cast<std::uint64_t>(environment.physical_cores));
    } else {
        json.field("physical_cores", std::int64_t{-1});
    }

    json.field("logical_processors", static_cast<std::uint64_t>(environment.logical_processors));
    json.field("smt", environment.smt);
    json.field("ram_bytes", environment.ram_bytes);
    json.field("os", environment.os);
    json.field("compiler", environment.compiler);
    json.field("build_config", environment.build_config);
    json.field("thunderbolt_version", environment.thunderbolt_version);

    // A measurement taken with assertions or ASan enabled is not a performance
    // result, and must not be quoted as one.
    json.field("debug_assertions", environment.debug_assertions);
    json.field("address_sanitizer", environment.address_sanitizer);

    json.field("timestamp_utc", environment.timestamp_utc);
    json.end_object();
}

void write_options(JsonWriter& json, const RunOptions& options) {
    json.begin_object("protocol");
    json.field("repetitions", options.repetitions);
    json.field("warmup_discarded", options.warmup);
    json.field("cooldown_ms", static_cast<std::uint64_t>(options.cooldown.count()));
    json.field("throttle_fraction", options.throttle_fraction);
    // Recorded explicitly so a reader never has to take the interleaving on
    // trust: a batched run would be a different, and invalid, protocol.
    json.field("interleaved", true);
    json.field("statistic", "median_and_iqr");
    json.end_object();
}

void write_summary(JsonWriter& json, std::string_view key, const Summary& summary) {
    json.begin_object(key);
    json.field("samples", static_cast<std::uint64_t>(summary.sample_count));
    json.field("median", summary.median);
    json.field("iqr", summary.iqr);
    json.field("p25", summary.p25);
    json.field("p75", summary.p75);
    json.field("min", summary.min);
    json.field("max", summary.max);
    json.field("mean", summary.mean);
    json.end_object();
}

void write_leg(JsonWriter& json, const LegResult& leg, bool include_samples) {
    json.begin_object();
    json.field("leg", leg.name);

    write_summary(json, "seconds", leg.timing);
    json.field("throttled_samples", static_cast<std::uint64_t>(leg.throttled_count));

    if (leg.throttled_count > 0) {
        // The unfiltered summary is kept so a reader can see how much excluding
        // throttled samples changed the answer.
        write_summary(json, "seconds_including_throttled", leg.timing_including_throttled);
    }

    if (!leg.counters.empty()) {
        json.begin_object("counters");
        for (const auto& counter : leg.counters) {
            json.field(counter.first, counter.second);
        }
        json.end_object();
    }

    if (include_samples) {
        json.begin_array("samples");
        for (const Sample& sample : leg.samples) {
            json.begin_object();
            json.field("seconds", sample.seconds);
            json.field("clock_mhz", sample.clock_mhz);
            json.field("throttled", sample.throttled);
            json.end_object();
        }
        json.end_array();
    }

    json.end_object();
}

void begin_result_document(JsonWriter& json, std::string_view benchmark_name,
                           const Environment& environment, const RunReport& report) {
    json.begin_object();
    json.field("schema", "thunderbolt-benchmark-1");
    json.field("benchmark", benchmark_name);
    write_environment(json, environment);
    write_options(json, report.options);

    json.begin_object("clock");
    json.field("nominal_max_mhz", report.nominal_max_mhz);
    // The reference for throttle flagging. A mobile part never holds its nominal
    // maximum under all-core load, so the useful question is whether a sample was
    // slow RELATIVE TO ITS PEERS.
    json.field("median_observed_mhz", report.median_observed_mhz);
    json.field("throttle_reference", "median_observed_mhz");
    // When frequency is unreadable, no claim may be made in either direction -
    // "not throttled" and "throttled" are equally unsupported.
    json.field("available", !report.clock_unavailable);
    json.end_object();
}

} // namespace thunderbolt::bench

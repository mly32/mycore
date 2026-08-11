#include "dots/server/input_provenance.hpp"

#include "dots/protocol/codec.hpp"

#include <fmt/format.h>
#include <string>

namespace dots::server {
namespace {

[[nodiscard]] std::optional<std::uint64_t> percentile(const InputProvenanceSummary& summary,
                                                      std::uint64_t numerator) noexcept {
    if (summary.applied_input_count == 0) {
        return std::nullopt;
    }
    const auto whole_hundreds = summary.applied_input_count / 100U;
    const auto remainder = summary.applied_input_count % 100U;
    const auto rank = whole_hundreds * numerator + (remainder * numerator + 99U) / 100U;
    auto cumulative = std::uint64_t{};
    for (auto bucket = std::size_t{}; bucket < summary.queue_wait_histogram.size(); ++bucket) {
        cumulative += summary.queue_wait_histogram[bucket];
        if (cumulative >= rank) {
            return static_cast<std::uint64_t>(bucket);
        }
    }
    return summary.maximum_queue_wait_ticks;
}

[[nodiscard]] std::string optional_json(std::optional<std::uint64_t> value) {
    return value ? std::to_string(*value) : "null";
}

[[nodiscard]] std::string histogram_json(const InputProvenanceSummary& summary) {
    std::string result{"["};
    for (auto index = std::size_t{}; index < summary.queue_wait_histogram.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        result += std::to_string(summary.queue_wait_histogram[index]);
    }
    result.push_back(']');
    return result;
}

} // namespace

InputQueueWaitStatistics
input_queue_wait_statistics(const InputProvenanceSummary& summary) noexcept {
    return {
        .count = summary.applied_input_count,
        .mean_ticks = summary.applied_input_count == 0
                          ? 0.0
                          : static_cast<double>(summary.queue_wait_tick_sum) /
                                static_cast<double>(summary.applied_input_count),
        .minimum_ticks = summary.minimum_queue_wait_ticks,
        .percentile_50_ticks = percentile(summary, 50),
        .percentile_95_ticks = percentile(summary, 95),
        .percentile_99_ticks = percentile(summary, 99),
        .maximum_ticks = summary.maximum_queue_wait_ticks,
    };
}

std::string input_provenance_header_jsonl(std::uint64_t maximum_applied_records) {
    return fmt::format("{{\"schema\":\"dots.authoritative-input-provenance\",\"schema_version\":1,"
                       "\"kind\":\"header\",\"protocol_version\":{},\"server_tick_rate_hz\":30,"
                       "\"receive_tick_semantics\":\"upcoming_authoritative_tick\","
                       "\"maximum_applied_records\":{}}}",
                       protocol::kProtocolVersion,
                       maximum_applied_records);
}

std::string input_provenance_record_jsonl(const InputProvenanceRecord& record) {
    return fmt::format("{{\"kind\":\"applied_input\",\"client_id\":{},\"input_sequence\":{},"
                       "\"client_tick\":{},\"receive_server_tick\":{},"
                       "\"application_server_tick\":{},\"queue_wait_ticks\":{}}}",
                       record.client_id.value(),
                       record.input_sequence.value(),
                       record.client_tick,
                       record.receive_server_tick,
                       record.application_server_tick,
                       record.queue_wait_ticks);
}

std::string input_provenance_summary_jsonl(const InputProvenanceArtifactSummary& summary) {
    const auto wait = input_queue_wait_statistics(summary.runtime);
    return fmt::format("{{\"kind\":\"summary\",\"complete\":{},\"final_server_tick\":{},"
                       "\"accepted_inputs\":{},\"applied_inputs\":{},"
                       "\"discarded_before_application\":{},\"pending_inputs\":{},"
                       "\"records_written\":{},\"records_omitted_by_limit\":{},"
                       "\"record_buffer_dropped\":{},\"queue_wait_tick_sum\":{},"
                       "\"queue_wait_min\":{},\"queue_wait_mean\":{:.6f},\"queue_wait_p50\":{},"
                       "\"queue_wait_p95\":{},\"queue_wait_p99\":{},\"queue_wait_max\":{},"
                       "\"queue_wait_histogram_0_to_63_then_64_plus\":{}}}",
                       summary.complete,
                       summary.final_server_tick,
                       summary.runtime.accepted_input_count,
                       summary.runtime.applied_input_count,
                       summary.runtime.discarded_before_application_count,
                       summary.runtime.pending_input_count,
                       summary.records_written,
                       summary.records_omitted_by_limit,
                       summary.runtime.record_buffer_dropped_count,
                       summary.runtime.queue_wait_tick_sum,
                       optional_json(wait.minimum_ticks),
                       wait.mean_ticks,
                       optional_json(wait.percentile_50_ticks),
                       optional_json(wait.percentile_95_ticks),
                       optional_json(wait.percentile_99_ticks),
                       optional_json(wait.maximum_ticks),
                       histogram_json(summary.runtime));
}

} // namespace dots::server

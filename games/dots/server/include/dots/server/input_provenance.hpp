#pragma once

#include "dots/protocol/ids.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace dots::server {

inline constexpr std::size_t kDefaultInputProvenanceRecordCapacity = 4'096;
inline constexpr std::size_t kInputProvenanceExactQueueWaitBucketCount = 64;
inline constexpr std::size_t kInputProvenanceQueueWaitBucketCount =
    kInputProvenanceExactQueueWaitBucketCount + 1;

struct InputProvenanceRecord {
    protocol::ClientId client_id;
    protocol::InputSequenceId input_sequence;
    std::uint32_t client_tick{};
    std::uint64_t receive_server_tick{};
    std::uint64_t application_server_tick{};
    std::uint64_t queue_wait_ticks{};

    bool operator==(const InputProvenanceRecord&) const = default;
};

struct InputProvenanceSummary {
    std::uint64_t accepted_input_count{};
    std::uint64_t applied_input_count{};
    std::uint64_t discarded_before_application_count{};
    std::uint64_t pending_input_count{};
    std::uint64_t record_buffer_dropped_count{};
    std::uint64_t queue_wait_tick_sum{};
    std::optional<std::uint64_t> minimum_queue_wait_ticks;
    std::optional<std::uint64_t> maximum_queue_wait_ticks;
    std::array<std::uint64_t, kInputProvenanceQueueWaitBucketCount> queue_wait_histogram{};

    bool operator==(const InputProvenanceSummary&) const = default;
};

struct InputQueueWaitStatistics {
    std::uint64_t count{};
    double mean_ticks{};
    std::optional<std::uint64_t> minimum_ticks;
    std::optional<std::uint64_t> percentile_50_ticks;
    std::optional<std::uint64_t> percentile_95_ticks;
    std::optional<std::uint64_t> percentile_99_ticks;
    std::optional<std::uint64_t> maximum_ticks;

    bool operator==(const InputQueueWaitStatistics&) const = default;
};

struct InputProvenanceArtifactSummary {
    InputProvenanceSummary runtime;
    std::uint64_t records_written{};
    std::uint64_t records_omitted_by_limit{};
    std::uint64_t final_server_tick{};
    bool complete{};

    bool operator==(const InputProvenanceArtifactSummary&) const = default;
};

[[nodiscard]] InputQueueWaitStatistics
input_queue_wait_statistics(const InputProvenanceSummary& summary) noexcept;

[[nodiscard]] std::string input_provenance_header_jsonl(std::uint64_t maximum_applied_records);
[[nodiscard]] std::string input_provenance_record_jsonl(const InputProvenanceRecord& record);
[[nodiscard]] std::string
input_provenance_summary_jsonl(const InputProvenanceArtifactSummary& summary);

} // namespace dots::server

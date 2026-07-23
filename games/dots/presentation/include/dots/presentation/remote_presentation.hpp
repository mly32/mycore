#pragma once

#include "dots/protocol/messages.hpp"
#include "mycore/math/vector2.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dots::presentation {

inline constexpr std::size_t kRemoteSnapshotCapacity = 32;
inline constexpr std::uint32_t kRemotePresentationDelayTicks = 6;

struct RemoteSnapshotSample {
    protocol::SnapshotId snapshot_id;
    std::uint32_t server_tick{};
    std::vector<protocol::EntityState> entities;
    std::chrono::steady_clock::time_point arrival_time;
};

struct RemoteEntitySample {
    protocol::EntityId entity_id;
    protocol::EntityKind kind{protocol::EntityKind::Player};
    mycore::math::Vector2 position;
    float mass{};
};

struct RemoteBracket {
    protocol::SnapshotId older_snapshot_id;
    std::uint32_t older_server_tick{};
    protocol::SnapshotId newer_snapshot_id;
    std::uint32_t newer_server_tick{};
    float alpha{};
};

struct RemotePresentationFrame {
    std::vector<RemoteEntitySample> entities;
    std::optional<RemoteBracket> bracket;
    double presentation_tick{};
    bool ready{};
    bool holding{};
};

struct RemoteEntityEndpoints {
    std::optional<RemoteEntitySample> older;
    std::optional<RemoteEntitySample> newer;
};

struct RemotePresentationStatistics {
    std::size_t sample_count{};
    std::size_t sample_capacity{kRemoteSnapshotCapacity};
    std::uint32_t coverage_ticks{};
    double coverage_milliseconds{};
    double target_delay_ticks{kRemotePresentationDelayTicks};
    double current_delay_ticks{};
    double presentation_tick{};
    double cursor_rate{1.0};
    double cursor_error{};
    std::optional<RemoteBracket> bracket;
    double latest_jitter_milliseconds{};
    double ewma_jitter_milliseconds{};
    std::uint64_t late_snapshot_count{};
    std::uint64_t hold_episode_count{};
    bool holding{};
    std::chrono::milliseconds current_hold_duration{};
    std::chrono::milliseconds last_hold_duration{};
    std::chrono::milliseconds maximum_hold_duration{};
    std::chrono::milliseconds total_hold_duration{};
    std::uint64_t hold_recovery_count{};
    std::uint64_t rate_correction_count{};
    std::uint64_t hard_rebase_count{};
    std::uint64_t delayed_entity_create_count{};
    std::uint64_t delayed_entity_remove_count{};
};

class RemoteSnapshotBuffer {
public:
    void insert(RemoteSnapshotSample sample);
    void advance(std::chrono::steady_clock::time_point now);

    // Sampling also records lifecycle transitions exposed to presentation.
    [[nodiscard]] RemotePresentationFrame sample(protocol::EntityId controlled_entity_id);
    [[nodiscard]] RemoteEntityEndpoints endpoints(protocol::EntityId entity_id) const;
    [[nodiscard]] RemotePresentationStatistics
    statistics(std::chrono::steady_clock::time_point now) const;

private:
    struct PresentedEntityIdentity {
        protocol::EntityId entity_id;
        protocol::EntityKind kind{protocol::EntityKind::Player};
    };

    [[nodiscard]] std::optional<std::size_t> older_sample_index() const noexcept;
    [[nodiscard]] std::optional<std::size_t> newer_sample_index() const noexcept;
    void finish_hold(std::chrono::steady_clock::time_point now) noexcept;
    void record_presented_lifecycle(const std::vector<RemoteEntitySample>& entities);

    std::vector<RemoteSnapshotSample> samples_;
    std::vector<PresentedEntityIdentity> last_presented_entities_;
    std::optional<std::chrono::steady_clock::time_point> last_advance_time_;
    std::optional<std::chrono::steady_clock::time_point> hold_started_at_;
    std::chrono::steady_clock::duration last_hold_duration_{};
    std::chrono::steady_clock::duration maximum_hold_duration_{};
    std::chrono::steady_clock::duration total_hold_duration_{};
    double presentation_tick_{};
    double cursor_rate_{1.0};
    double cursor_error_{};
    double latest_jitter_milliseconds_{};
    double ewma_jitter_milliseconds_{};
    std::uint64_t late_snapshot_count_{};
    std::uint64_t hold_episode_count_{};
    std::uint64_t hold_recovery_count_{};
    std::uint64_t rate_correction_count_{};
    std::uint64_t hard_rebase_count_{};
    std::uint64_t delayed_entity_create_count_{};
    std::uint64_t delayed_entity_remove_count_{};
    bool ready_{};
    bool rate_correction_active_{};
    bool lifecycle_initialized_{};
};

} // namespace dots::presentation

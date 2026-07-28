#pragma once

#include "dots/protocol/ids.hpp"
#include "dots/protocol/messages.hpp"
#include "dots/replication/replication.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace dots::client_runtime {

enum class State : std::uint8_t {
    Connecting,
    Handshaking,
    Ready,
    Disconnected,
    Failed,
};

enum class RuntimeError : std::uint8_t {
    MultipleConnections,
    ProtocolEncodeFailed,
    ProtocolDecodeFailed,
    UnexpectedMessage,
    InvalidSnapshot,
    InvalidInputAcknowledgement,
    MissingControlledEntity,
    TransportSendFailed,
};

enum class InputSendResult : std::uint8_t {
    Sent,
    NotReady,
    InvalidMovement,
    InvalidAction,
    InvalidClientTick,
    SequenceExhausted,
    TransportFailure,
};

struct ReplicationStatistics {
    std::optional<std::chrono::milliseconds> latest_snapshot_age;
    float accepted_snapshots_per_second{};
    std::uint64_t accepted_snapshot_count{};
};

struct Settings {
    bool input_redundancy{true};
};

struct AcceptedSnapshot {
    protocol::FullSnapshot snapshot;
    std::chrono::steady_clock::time_point arrival_time;
};

struct ProcessEventsResult {
    std::optional<RuntimeError> error;
    std::vector<AcceptedSnapshot> accepted_snapshots;

    [[nodiscard]] bool has_value() const noexcept {
        return error.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.has_value();
    }
};

inline constexpr std::size_t kPredictionHistoryCapacity = 256;

struct PredictionStatistics {
    bool input_redundancy_enabled{true};
    protocol::InputSequenceId last_input_sent;
    protocol::InputSequenceId last_input_acknowledged;
    std::size_t unacknowledged_input_count{};
    std::size_t history_count{};
    std::size_t history_capacity{kPredictionHistoryCapacity};
    std::size_t history_high_water_mark{};
    std::uint8_t latest_server_pending_input_count{};
    std::uint8_t server_pending_input_high_water_mark{};
    protocol::SnapshotId rollback_snapshot_id;
    std::uint32_t rollback_server_tick{};
    protocol::InputSequenceId rollback_input_acknowledgement;
    std::size_t latest_replay_count{};
    std::uint64_t total_replayed_input_count{};
    std::size_t maximum_replay_count{};
    double latest_replay_milliseconds{};
    double average_replay_milliseconds{};
    double maximum_replay_milliseconds{};
    std::uint64_t reconciliation_count{};
    std::uint64_t nonzero_correction_count{};
    float latest_correction_distance{};
    float maximum_correction_distance{};
    float corrections_per_minute{};
    mycore::math::Vector2 accumulated_correction_displacement;
    std::uint64_t correction_sequence_since_hard_resync{};
    std::uint64_t replay_over_budget_count{};
    std::uint64_t hard_resync_count{};
    std::size_t pending_injected_input_drop_count{};
    std::uint64_t injected_input_drop_count{};
    std::uint64_t injected_prediction_error_count{};
};

class Runtime {
public:
    explicit Runtime(mycore::net_transport::Endpoint& endpoint, Settings settings = {});
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;

    [[nodiscard]] ProcessEventsResult
    process_events(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    [[nodiscard]] InputSendResult send_input(std::uint32_t client_tick,
                                             mycore::math::Vector2 movement,
                                             std::uint16_t action_bits = 0);
    [[nodiscard]] bool disconnect();

    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] const replication::ReplicatedWorld& world() const noexcept;
    [[nodiscard]] protocol::ClientId client_id() const noexcept;
    [[nodiscard]] protocol::EntityId controlled_entity_id() const noexcept;
    [[nodiscard]] protocol::SessionMode session_mode() const noexcept;
    [[nodiscard]] std::span<const protocol::EntityId> owned_entity_ids() const noexcept;
    [[nodiscard]] protocol::EntityId primary_entity_id() const noexcept;
    [[nodiscard]] protocol::EntityId follow_entity_id() const noexcept;
    [[nodiscard]] std::optional<protocol::WorldRules> world_rules() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> defeat_tick() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> respawn_available_tick() const noexcept;
    [[nodiscard]] std::uint32_t respawn_cooldown_ticks() const noexcept;
    [[nodiscard]] std::optional<protocol::PlayerAbsorbed> latest_absorption() const noexcept;
    [[nodiscard]] protocol::InputSequenceId latest_respawn_request_id() const noexcept;
    [[nodiscard]] protocol::RespawnResult latest_respawn_result() const noexcept;
    [[nodiscard]] mycore::net_transport::ConnectionHandle connection_handle() const noexcept;
    [[nodiscard]] std::optional<mycore::math::Vector2> predicted_position() const noexcept;
    [[nodiscard]] std::optional<mycore::math::Vector2> pre_correction_position() const noexcept;
    [[nodiscard]] std::span<const mycore::math::Vector2> latest_replay_path() const noexcept;
    [[nodiscard]] std::span<const mycore::math::Vector2>
    latest_correction_replay_path() const noexcept;
    [[nodiscard]] bool debug_inject_prediction_error(mycore::math::Vector2 displacement);
    [[nodiscard]] bool debug_drop_next_input_packets(std::size_t count);
    [[nodiscard]] PredictionStatistics
    prediction_statistics(std::chrono::steady_clock::time_point now =
                              std::chrono::steady_clock::now()) const noexcept;
    [[nodiscard]] ReplicationStatistics
    replication_statistics(std::chrono::steady_clock::time_point now =
                               std::chrono::steady_clock::now()) const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dots::client_runtime

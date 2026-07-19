#pragma once

#include "dots/protocol/ids.hpp"
#include "dots/replication/replication.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

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
    std::uint64_t replay_over_budget_count{};
    std::uint64_t hard_resync_count{};
};

class Runtime {
public:
    explicit Runtime(mycore::net_transport::Endpoint& endpoint, Settings settings = {});
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;

    [[nodiscard]] std::optional<RuntimeError>
    process_events(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    [[nodiscard]] InputSendResult send_input(std::uint32_t client_tick,
                                             mycore::math::Vector2 movement);
    [[nodiscard]] bool disconnect();

    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] const replication::ReplicatedWorld& world() const noexcept;
    [[nodiscard]] protocol::ClientId client_id() const noexcept;
    [[nodiscard]] protocol::EntityId controlled_entity_id() const noexcept;
    [[nodiscard]] mycore::net_transport::ConnectionHandle connection_handle() const noexcept;
    [[nodiscard]] std::optional<mycore::math::Vector2> predicted_position() const noexcept;
    [[nodiscard]] std::optional<mycore::math::Vector2> pre_correction_position() const noexcept;
    [[nodiscard]] std::span<const mycore::math::Vector2> latest_replay_path() const noexcept;
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

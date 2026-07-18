#pragma once

#include "dots/protocol/ids.hpp"
#include "dots/replication/replication.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

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
    MissingControlledEntity,
    TransportSendFailed,
};

enum class InputSendResult : std::uint8_t {
    Sent,
    NotReady,
    InvalidMovement,
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
    [[nodiscard]] ReplicationStatistics
    replication_statistics(std::chrono::steady_clock::time_point now =
                               std::chrono::steady_clock::now()) const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dots::client_runtime

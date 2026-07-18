#pragma once

#include "mycore/core/strong_id.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mycore::net_transport {

struct ConnectionHandleTag;
using ConnectionHandle = mycore::core::StrongId<ConnectionHandleTag, std::uint32_t>;

enum class DeliveryMode : std::uint8_t {
    Reliable,
    Unreliable,
};

enum class DisconnectReason : std::uint8_t {
    LocalRequest,
    RemoteRequest,
    TransportFailure,
};

enum class SendStatus : std::uint8_t {
    Sent,
    UnknownConnection,
    Disconnected,
    PayloadTooLarge,
    QueueFull,
    TransportFailure,
};

enum class ConnectionState : std::uint8_t {
    Connecting,
    Connected,
    Closing,
    Disconnected,
    Failed,
};

struct TransportStatistics {
    ConnectionState state{};
    std::optional<std::chrono::milliseconds> round_trip_time;
    std::optional<float> packet_loss_percent;
    std::optional<float> outbound_packets_per_second;
    std::optional<float> outbound_bytes_per_second;
    std::optional<float> inbound_packets_per_second;
    std::optional<float> inbound_bytes_per_second;
    std::optional<std::size_t> pending_unreliable_bytes;
    std::optional<std::size_t> pending_reliable_bytes;
    std::optional<std::size_t> sent_unacknowledged_reliable_bytes;
    std::optional<std::chrono::microseconds> outbound_queue_delay;
};

struct NetworkImpairment {
    std::uint32_t outgoing_lag_milliseconds{};
    float outgoing_loss_percent{};
};

class NetworkAddress {
public:
    [[nodiscard]] static std::optional<NetworkAddress> parse(std::string_view value);

    [[nodiscard]] const std::string& value() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    explicit NetworkAddress(std::string value);

    std::string value_;
};

struct Connected {
    ConnectionHandle connection;
};

struct Disconnected {
    ConnectionHandle connection;
    DisconnectReason reason{};
};

struct PayloadReceived {
    ConnectionHandle connection;
    DeliveryMode delivery{};
    std::vector<std::byte> payload;
};

using Event = std::variant<Connected, Disconnected, PayloadReceived>;

class Endpoint {
public:
    virtual ~Endpoint() = default;

    [[nodiscard]] virtual std::vector<Event> poll() = 0;
    [[nodiscard]] virtual SendStatus send(ConnectionHandle connection,
                                          std::span<const std::byte> payload,
                                          DeliveryMode delivery) = 0;
    [[nodiscard]] virtual bool disconnect(ConnectionHandle connection) = 0;
    [[nodiscard]] virtual std::optional<TransportStatistics>
    statistics(ConnectionHandle connection) const = 0;
};

class InMemoryNetwork {
public:
    InMemoryNetwork();
    ~InMemoryNetwork();

    InMemoryNetwork(const InMemoryNetwork&) = delete;
    InMemoryNetwork& operator=(const InMemoryNetwork&) = delete;
    InMemoryNetwork(InMemoryNetwork&&) noexcept;
    InMemoryNetwork& operator=(InMemoryNetwork&&) noexcept;

    [[nodiscard]] Endpoint& server_endpoint() noexcept;
    [[nodiscard]] Endpoint& connect_client();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct ListeningEndpoint {
    Endpoint* endpoint{};
    NetworkAddress address;
};

class GameNetworkingSocketsNetwork {
public:
    explicit GameNetworkingSocketsNetwork(NetworkImpairment impairment = {});
    ~GameNetworkingSocketsNetwork();

    GameNetworkingSocketsNetwork(const GameNetworkingSocketsNetwork&) = delete;
    GameNetworkingSocketsNetwork& operator=(const GameNetworkingSocketsNetwork&) = delete;
    GameNetworkingSocketsNetwork(GameNetworkingSocketsNetwork&&) = delete;
    GameNetworkingSocketsNetwork& operator=(GameNetworkingSocketsNetwork&&) = delete;

    [[nodiscard]] ListeningEndpoint listen(const NetworkAddress& address);
    [[nodiscard]] Endpoint& connect(const NetworkAddress& address);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mycore::net_transport

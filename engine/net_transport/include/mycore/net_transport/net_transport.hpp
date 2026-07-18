#pragma once

#include "mycore/core/strong_id.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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

} // namespace mycore::net_transport

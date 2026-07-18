#include "mycore/net_transport/net_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mycore::net_transport {
namespace {

class InMemoryEndpoint;

struct Connection {
    ConnectionHandle handle;
    InMemoryEndpoint* client{};
    bool connected{true};
};

struct SharedState {
    InMemoryEndpoint* server{};
    std::unordered_map<std::uint32_t, Connection> connections;
};

class InMemoryEndpoint final : public Endpoint {
public:
    InMemoryEndpoint(SharedState& state, bool server)
        : state_(state),
          server_(server) {}

    [[nodiscard]] std::vector<Event> poll() override {
        std::vector<Event> events;
        events.reserve(events_.size());
        while (!events_.empty()) {
            events.push_back(std::move(events_.front()));
            events_.pop_front();
        }
        return events;
    }

    [[nodiscard]] SendStatus send(ConnectionHandle connection,
                                  std::span<const std::byte> payload,
                                  DeliveryMode delivery) override {
        auto* value = find_connection(connection);
        if (value == nullptr) {
            return SendStatus::UnknownConnection;
        }
        if (!value->connected) {
            return SendStatus::Disconnected;
        }

        auto* peer = server_ ? value->client : state_.server;
        peer->events_.push_back(PayloadReceived{
            .connection = connection,
            .delivery = delivery,
            .payload = {payload.begin(), payload.end()},
        });
        return SendStatus::Sent;
    }

    [[nodiscard]] bool disconnect(ConnectionHandle connection) override {
        auto* value = find_connection(connection);
        if (value == nullptr || !value->connected) {
            return false;
        }
        value->connected = false;

        auto* peer = server_ ? value->client : state_.server;
        events_.push_back(Disconnected{
            .connection = connection,
            .reason = DisconnectReason::LocalRequest,
        });
        peer->events_.push_back(Disconnected{
            .connection = connection,
            .reason = DisconnectReason::RemoteRequest,
        });
        return true;
    }

    [[nodiscard]] std::optional<TransportStatistics>
    statistics(ConnectionHandle connection) const override {
        const auto* value = find_connection(connection);
        if (value == nullptr) {
            return std::nullopt;
        }
        TransportStatistics result;
        result.state =
            value->connected ? ConnectionState::Connected : ConnectionState::Disconnected;
        return result;
    }

    void push(Event event) {
        events_.push_back(std::move(event));
    }

    void set_client_connection(ConnectionHandle connection) noexcept {
        client_connection_ = connection;
    }

private:
    [[nodiscard]] Connection* find_connection(ConnectionHandle connection) noexcept {
        if (!connection.is_valid() || (!server_ && connection != client_connection_)) {
            return nullptr;
        }
        const auto iterator = state_.connections.find(connection.value());
        return iterator == state_.connections.end() ? nullptr : &iterator->second;
    }

    [[nodiscard]] const Connection* find_connection(ConnectionHandle connection) const noexcept {
        if (!connection.is_valid() || (!server_ && connection != client_connection_)) {
            return nullptr;
        }
        const auto iterator = state_.connections.find(connection.value());
        return iterator == state_.connections.end() ? nullptr : &iterator->second;
    }

    SharedState& state_;
    bool server_{};
    ConnectionHandle client_connection_;
    std::deque<Event> events_;
};

} // namespace

class InMemoryNetwork::Impl {
public:
    Impl()
        : server(state_, true) {
        state_.server = &server;
    }

    [[nodiscard]] Endpoint& connect_client() {
        if (next_connection_ == ConnectionHandle::kInvalidValue) {
            throw std::runtime_error{"In-memory transport connection handles are exhausted"};
        }

        const ConnectionHandle handle{next_connection_++};
        auto client = std::make_unique<InMemoryEndpoint>(state_, false);
        client->set_client_connection(handle);
        auto* client_pointer = client.get();
        state_.connections.emplace(handle.value(),
                                   Connection{
                                       .handle = handle,
                                       .client = client_pointer,
                                   });
        server.push(Connected{.connection = handle});
        client->push(Connected{.connection = handle});
        clients.push_back(std::move(client));
        return *client_pointer;
    }

    SharedState state_;
    InMemoryEndpoint server;
    std::vector<std::unique_ptr<InMemoryEndpoint>> clients;
    std::uint32_t next_connection_{};
};

InMemoryNetwork::InMemoryNetwork()
    : impl_(std::make_unique<Impl>()) {}

InMemoryNetwork::~InMemoryNetwork() = default;
InMemoryNetwork::InMemoryNetwork(InMemoryNetwork&&) noexcept = default;
InMemoryNetwork& InMemoryNetwork::operator=(InMemoryNetwork&&) noexcept = default;

Endpoint& InMemoryNetwork::server_endpoint() noexcept {
    return impl_->server;
}

Endpoint& InMemoryNetwork::connect_client() {
    return impl_->connect_client();
}

} // namespace mycore::net_transport

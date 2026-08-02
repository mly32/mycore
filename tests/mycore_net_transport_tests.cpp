#include "mycore/net_transport/net_transport.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct NativeConnectionPair {
    mycore::net_transport::ConnectionHandle server;
    mycore::net_transport::ConnectionHandle client;
};

NativeConnectionPair wait_for_native_connection(mycore::net_transport::Endpoint& server,
                                                mycore::net_transport::Endpoint& client) {
    NativeConnectionPair pair;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           (!pair.server.is_valid() || !pair.client.is_valid())) {
        for (const auto& event : server.poll()) {
            if (const auto* connected = std::get_if<mycore::net_transport::Connected>(&event)) {
                pair.server = connected->connection;
            }
        }
        for (const auto& event : client.poll()) {
            if (const auto* connected = std::get_if<mycore::net_transport::Connected>(&event)) {
                pair.client = connected->connection;
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(pair.server.is_valid());
    REQUIRE(pair.client.is_valid());
    return pair;
}

} // namespace

TEST_CASE("Native transport accepts only numeric network addresses", "[transport][address]") {
    const auto ipv4 = mycore::net_transport::NetworkAddress::parse("127.0.0.1:27020");
    REQUIRE(ipv4.has_value());
    CHECK(ipv4->port() == 27020);

    const auto ipv6 = mycore::net_transport::NetworkAddress::parse("[::1]:27020");
    REQUIRE(ipv6.has_value());
    CHECK(ipv6->port() == 27020);

    CHECK_FALSE(mycore::net_transport::NetworkAddress::parse("localhost:27020").has_value());
    CHECK_FALSE(mycore::net_transport::NetworkAddress::parse("127.0.0.1").has_value());
}

TEST_CASE("Native transport rejects invalid impairment settings", "[transport][impairment]") {
    CHECK_THROWS_AS(mycore::net_transport::GameNetworkingSocketsNetwork({
                        .outgoing_loss_percent = 100.1F,
                    }),
                    std::invalid_argument);
    CHECK_THROWS_AS(mycore::net_transport::GameNetworkingSocketsNetwork({
                        .outgoing_loss_percent = std::numeric_limits<float>::infinity(),
                    }),
                    std::invalid_argument);
}

TEST_CASE("In-memory transport connects multiple isolated clients", "[transport][in-memory]") {
    mycore::net_transport::InMemoryNetwork network;
    auto& first = network.connect_client();
    auto& second = network.connect_client();

    const auto server_events = network.server_endpoint().poll();
    const auto first_events = first.poll();
    const auto second_events = second.poll();
    REQUIRE(server_events.size() == 2);
    REQUIRE(first_events.size() == 1);
    REQUIRE(second_events.size() == 1);

    const auto first_connection =
        std::get<mycore::net_transport::Connected>(first_events[0]).connection;
    const auto second_connection =
        std::get<mycore::net_transport::Connected>(second_events[0]).connection;
    REQUIRE(first_connection != second_connection);

    const std::array payload{std::byte{1}, std::byte{2}};
    REQUIRE(
        first.send(first_connection, payload, mycore::net_transport::DeliveryMode::Unreliable) ==
        mycore::net_transport::SendStatus::Sent);
    REQUIRE(network.server_endpoint().send(
                second_connection, payload, mycore::net_transport::DeliveryMode::Reliable) ==
            mycore::net_transport::SendStatus::Sent);

    const auto received_by_server = network.server_endpoint().poll();
    const auto received_by_first = first.poll();
    const auto received_by_second = second.poll();
    REQUIRE(received_by_server.size() == 1);
    REQUIRE(received_by_first.empty());
    REQUIRE(received_by_second.size() == 1);
    CHECK(std::get<mycore::net_transport::PayloadReceived>(received_by_server[0]).connection ==
          first_connection);
    CHECK(std::get<mycore::net_transport::PayloadReceived>(received_by_second[0]).delivery ==
          mycore::net_transport::DeliveryMode::Reliable);
}

TEST_CASE("In-memory transport copies payloads and preserves FIFO order",
          "[transport][in-memory]") {
    mycore::net_transport::InMemoryNetwork network;
    auto& client = network.connect_client();
    const auto connection =
        std::get<mycore::net_transport::Connected>(client.poll().front()).connection;
    static_cast<void>(network.server_endpoint().poll());

    std::array first_payload{std::byte{1}};
    const std::array second_payload{std::byte{2}};
    REQUIRE(client.send(connection, first_payload, mycore::net_transport::DeliveryMode::Reliable) ==
            mycore::net_transport::SendStatus::Sent);
    first_payload[0] = std::byte{9};
    REQUIRE(
        client.send(connection, second_payload, mycore::net_transport::DeliveryMode::Unreliable) ==
        mycore::net_transport::SendStatus::Sent);

    const auto events = network.server_endpoint().poll();
    REQUIRE(events.size() == 2);
    CHECK(std::get<mycore::net_transport::PayloadReceived>(events[0]).payload[0] == std::byte{1});
    CHECK(std::get<mycore::net_transport::PayloadReceived>(events[1]).payload[0] == std::byte{2});
}

TEST_CASE("In-memory transport disconnects both sides exactly once", "[transport][in-memory]") {
    mycore::net_transport::InMemoryNetwork network;
    auto& client = network.connect_client();
    const auto connection =
        std::get<mycore::net_transport::Connected>(client.poll().front()).connection;
    static_cast<void>(network.server_endpoint().poll());

    REQUIRE(client.disconnect(connection));
    REQUIRE_FALSE(client.disconnect(connection));
    const std::array payload{std::byte{1}};
    REQUIRE(client.send(connection, payload, mycore::net_transport::DeliveryMode::Reliable) ==
            mycore::net_transport::SendStatus::Disconnected);

    const auto client_events = client.poll();
    const auto server_events = network.server_endpoint().poll();
    REQUIRE(client_events.size() == 1);
    REQUIRE(server_events.size() == 1);
    CHECK(std::get<mycore::net_transport::Disconnected>(client_events[0]).reason ==
          mycore::net_transport::DisconnectReason::LocalRequest);
    CHECK(std::get<mycore::net_transport::Disconnected>(server_events[0]).reason ==
          mycore::net_transport::DisconnectReason::RemoteRequest);
    CHECK(client.poll().empty());
    CHECK(network.server_endpoint().poll().empty());
}

TEST_CASE("In-memory transport reports state without native measurements",
          "[transport][in-memory][statistics]") {
    mycore::net_transport::InMemoryNetwork network;
    auto& client = network.connect_client();
    const auto connection =
        std::get<mycore::net_transport::Connected>(client.poll().front()).connection;

    const auto statistics = client.statistics(connection);
    REQUIRE(statistics.has_value());
    CHECK(statistics->state == mycore::net_transport::ConnectionState::Connected);
    CHECK_FALSE(statistics->round_trip_time.has_value());
    CHECK_FALSE(statistics->packet_loss_percent.has_value());
    CHECK_FALSE(statistics->pending_reliable_bytes.has_value());
}

TEST_CASE("Native transport connects and preserves message delivery intent",
          "[transport][native][loopback]") {
    const auto listen_address = mycore::net_transport::NetworkAddress::parse("127.0.0.1:0");
    REQUIRE(listen_address.has_value());

    mycore::net_transport::GameNetworkingSocketsNetwork network;
    const auto listening = network.listen(*listen_address);
    REQUIRE(listening.endpoint != nullptr);
    auto& client = network.connect(listening.address);
    const auto pair = wait_for_native_connection(*listening.endpoint, client);

    const std::array reliable{std::byte{1}, std::byte{2}};
    const std::array unreliable{std::byte{3}};
    REQUIRE(client.send(pair.client, reliable, mycore::net_transport::DeliveryMode::Reliable) ==
            mycore::net_transport::SendStatus::Sent);
    REQUIRE(listening.endpoint->send(
                pair.server, unreliable, mycore::net_transport::DeliveryMode::Unreliable) ==
            mycore::net_transport::SendStatus::Sent);

    std::vector<mycore::net_transport::PayloadReceived> server_payloads;
    std::vector<mycore::net_transport::PayloadReceived> client_payloads;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           (server_payloads.empty() || client_payloads.empty())) {
        for (const auto& event : listening.endpoint->poll()) {
            if (const auto* payload = std::get_if<mycore::net_transport::PayloadReceived>(&event)) {
                server_payloads.push_back(*payload);
            }
        }
        for (const auto& event : client.poll()) {
            if (const auto* payload = std::get_if<mycore::net_transport::PayloadReceived>(&event)) {
                client_payloads.push_back(*payload);
            }
        }
        std::this_thread::sleep_for(1ms);
    }

    REQUIRE(server_payloads.size() == 1);
    CHECK(server_payloads.front().delivery == mycore::net_transport::DeliveryMode::Reliable);
    CHECK(server_payloads.front().payload == std::vector<std::byte>{std::byte{1}, std::byte{2}});
    REQUIRE(client_payloads.size() == 1);
    CHECK(client_payloads.front().delivery == mycore::net_transport::DeliveryMode::Unreliable);

    const auto statistics = client.statistics(pair.client);
    REQUIRE(statistics.has_value());
    CHECK(statistics->state == mycore::net_transport::ConnectionState::Connected);
    CHECK(statistics->round_trip_time.has_value());
    CHECK(statistics->pending_reliable_bytes.has_value());

    REQUIRE(client.disconnect(pair.client));
    REQUIRE_FALSE(client.disconnect(pair.client));
    const auto local_events = client.poll();
    REQUIRE(local_events.size() == 1);
    CHECK(std::get<mycore::net_transport::Disconnected>(local_events.front()).reason ==
          mycore::net_transport::DisconnectReason::LocalRequest);

    bool remote_disconnected{};
    const auto disconnect_deadline = std::chrono::steady_clock::now() + 5s;
    while (!remote_disconnected && std::chrono::steady_clock::now() < disconnect_deadline) {
        for (const auto& event : listening.endpoint->poll()) {
            if (const auto* disconnected =
                    std::get_if<mycore::net_transport::Disconnected>(&event)) {
                CHECK(disconnected->reason ==
                      mycore::net_transport::DisconnectReason::RemoteRequest);
                remote_disconnected = true;
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    CHECK(remote_disconnected);
    CHECK_FALSE(listening.endpoint->statistics(pair.server).has_value());
    REQUIRE(client.statistics(pair.client).has_value());
    CHECK(client.statistics(pair.client)->state ==
          mycore::net_transport::ConnectionState::Disconnected);
}

TEST_CASE("Native loopback statistics reflect simulated latency",
          "[transport][native][statistics][impairment]") {
    using namespace std::chrono_literals;
    const auto listen_address = mycore::net_transport::NetworkAddress::parse("127.0.0.1:0");
    REQUIRE(listen_address.has_value());
    mycore::net_transport::GameNetworkingSocketsNetwork network{{
        .outgoing_lag_milliseconds = 15,
    }};
    const auto listening = network.listen(*listen_address);
    auto& client = network.connect(listening.address);
    const auto pair = wait_for_native_connection(*listening.endpoint, client);

    std::optional<mycore::net_transport::TransportStatistics> statistics;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(listening.endpoint->poll());
        static_cast<void>(client.poll());
        statistics = client.statistics(pair.client);
        if (statistics && statistics->round_trip_time && *statistics->round_trip_time >= 15ms) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(statistics.has_value());
    REQUIRE(statistics->round_trip_time.has_value());
    CHECK(*statistics->round_trip_time >= 15ms);
}

TEST_CASE("Native transport accepts multiple independently routed clients",
          "[transport][native][multiple]") {
    using namespace std::chrono_literals;
    const auto bind = mycore::net_transport::NetworkAddress::parse("127.0.0.1:0");
    REQUIRE(bind.has_value());
    mycore::net_transport::GameNetworkingSocketsNetwork network;
    const auto listening = network.listen(*bind);
    auto& first = network.connect(listening.address);
    auto& second = network.connect(listening.address);

    std::vector<mycore::net_transport::ConnectionHandle> server_connections;
    mycore::net_transport::ConnectionHandle first_connection;
    mycore::net_transport::ConnectionHandle second_connection;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           (server_connections.size() < 2 || !first_connection.is_valid() ||
            !second_connection.is_valid())) {
        for (const auto& event : listening.endpoint->poll()) {
            if (const auto* connected = std::get_if<mycore::net_transport::Connected>(&event)) {
                server_connections.push_back(connected->connection);
            }
        }
        for (const auto& event : first.poll()) {
            if (const auto* connected = std::get_if<mycore::net_transport::Connected>(&event)) {
                first_connection = connected->connection;
            }
        }
        for (const auto& event : second.poll()) {
            if (const auto* connected = std::get_if<mycore::net_transport::Connected>(&event)) {
                second_connection = connected->connection;
            }
        }
        std::this_thread::sleep_for(1ms);
    }

    REQUIRE(server_connections.size() == 2);
    REQUIRE(first_connection.is_valid());
    REQUIRE(second_connection.is_valid());
    CHECK(first_connection != second_connection);

    const std::array first_payload{std::byte{1}};
    const std::array second_payload{std::byte{2}};
    REQUIRE(first.send(
                first_connection, first_payload, mycore::net_transport::DeliveryMode::Unreliable) ==
            mycore::net_transport::SendStatus::Sent);
    REQUIRE(second.send(second_connection,
                        second_payload,
                        mycore::net_transport::DeliveryMode::Unreliable) ==
            mycore::net_transport::SendStatus::Sent);

    std::vector<mycore::net_transport::PayloadReceived> received;
    const auto delivery_deadline = std::chrono::steady_clock::now() + 5s;
    while (received.size() < 2 && std::chrono::steady_clock::now() < delivery_deadline) {
        for (const auto& event : listening.endpoint->poll()) {
            if (const auto* payload = std::get_if<mycore::net_transport::PayloadReceived>(&event)) {
                received.push_back(*payload);
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(received.size() == 2);
    CHECK(received[0].connection != received[1].connection);
    CHECK((received[0].payload == std::vector<std::byte>{std::byte{1}} ||
           received[1].payload == std::vector<std::byte>{std::byte{1}}));
    CHECK((received[0].payload == std::vector<std::byte>{std::byte{2}} ||
           received[1].payload == std::vector<std::byte>{std::byte{2}}));
}

TEST_CASE("Native transport drains immediate reliable messages during connection fan-in",
          "[transport][native][multiple][handshake]") {
    constexpr auto kClientCount = std::size_t{12};
    const auto bind = mycore::net_transport::NetworkAddress::parse("127.0.0.1:0");
    REQUIRE(bind.has_value());
    mycore::net_transport::GameNetworkingSocketsNetwork network;
    const auto listening = network.listen(*bind);
    std::vector<mycore::net_transport::Endpoint*> clients;
    clients.reserve(kClientCount);
    for (auto index = std::size_t{}; index < kClientCount; ++index) {
        clients.push_back(&network.connect(listening.address));
    }

    std::vector<mycore::net_transport::ConnectionHandle> client_connections(kClientCount);
    std::vector<mycore::net_transport::ConnectionHandle> server_connections;
    std::vector<std::byte> received_payloads;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           (server_connections.size() < kClientCount || received_payloads.size() < kClientCount)) {
        for (const auto& event : listening.endpoint->poll()) {
            if (const auto* connected = std::get_if<mycore::net_transport::Connected>(&event)) {
                server_connections.push_back(connected->connection);
            } else if (const auto* payload =
                           std::get_if<mycore::net_transport::PayloadReceived>(&event)) {
                REQUIRE(payload->delivery == mycore::net_transport::DeliveryMode::Reliable);
                REQUIRE(payload->payload.size() == 1);
                received_payloads.push_back(payload->payload.front());
            }
        }
        for (auto index = std::size_t{}; index < clients.size(); ++index) {
            for (const auto& event : clients[index]->poll()) {
                const auto* connected = std::get_if<mycore::net_transport::Connected>(&event);
                if (connected == nullptr || client_connections[index].is_valid()) {
                    continue;
                }
                client_connections[index] = connected->connection;
                const std::array payload{std::byte{static_cast<unsigned char>(index)}};
                REQUIRE(clients[index]->send(connected->connection,
                                             payload,
                                             mycore::net_transport::DeliveryMode::Reliable) ==
                        mycore::net_transport::SendStatus::Sent);
            }
        }
        std::this_thread::sleep_for(1ms);
    }

    REQUIRE(server_connections.size() == kClientCount);
    REQUIRE(received_payloads.size() == kClientCount);
    std::ranges::sort(received_payloads);
    for (auto index = std::size_t{}; index < received_payloads.size(); ++index) {
        CHECK(received_payloads[index] == std::byte{static_cast<unsigned char>(index)});
    }
}

TEST_CASE("Native transport drains reliable data before a lingered remote close",
          "[transport][native][disconnect]") {
    const auto listen_address = mycore::net_transport::NetworkAddress::parse("127.0.0.1:0");
    REQUIRE(listen_address.has_value());

    mycore::net_transport::GameNetworkingSocketsNetwork network{{
        .outgoing_lag_milliseconds = 20,
    }};
    const auto listening = network.listen(*listen_address);
    auto& client = network.connect(listening.address);
    const auto pair = wait_for_native_connection(*listening.endpoint, client);

    const std::array payload{std::byte{1}, std::byte{2}, std::byte{3}};
    REQUIRE(client.send(pair.client, payload, mycore::net_transport::DeliveryMode::Reliable) ==
            mycore::net_transport::SendStatus::Sent);
    REQUIRE(client.disconnect(pair.client));

    std::vector<mycore::net_transport::Event> remote_events;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           std::none_of(remote_events.begin(), remote_events.end(), [](const auto& event) {
               return std::holds_alternative<mycore::net_transport::Disconnected>(event);
           })) {
        auto events = listening.endpoint->poll();
        remote_events.insert(remote_events.end(),
                             std::make_move_iterator(events.begin()),
                             std::make_move_iterator(events.end()));
        static_cast<void>(client.poll());
        std::this_thread::sleep_for(1ms);
    }

    const auto payload_event =
        std::find_if(remote_events.begin(), remote_events.end(), [](const auto& event) {
            return std::holds_alternative<mycore::net_transport::PayloadReceived>(event);
        });
    const auto disconnect_event =
        std::find_if(remote_events.begin(), remote_events.end(), [](const auto& event) {
            return std::holds_alternative<mycore::net_transport::Disconnected>(event);
        });
    REQUIRE(payload_event != remote_events.end());
    REQUIRE(disconnect_event != remote_events.end());
    CHECK(payload_event < disconnect_event);
    CHECK(std::get<mycore::net_transport::PayloadReceived>(*payload_event).payload ==
          std::vector<std::byte>{payload.begin(), payload.end()});
}

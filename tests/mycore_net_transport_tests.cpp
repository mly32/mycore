#include "mycore/net_transport/net_transport.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <variant>

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

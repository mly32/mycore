#include "dots/client_runtime/client_runtime.hpp"
#include "dots/protocol/codec.hpp"
#include "dots/server/server_runtime.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <span>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

void complete_handshake(dots::client_runtime::Runtime& client, dots::server::Runtime& server) {
    REQUIRE_FALSE(client.process_events().has_value());
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(client.process_events().has_value());
    REQUIRE(client.state() == dots::client_runtime::State::Ready);
}

class ManualEndpoint final : public mycore::net_transport::Endpoint {
public:
    [[nodiscard]] std::vector<mycore::net_transport::Event> poll() override {
        return std::exchange(events, {});
    }

    [[nodiscard]] mycore::net_transport::SendStatus
    send(mycore::net_transport::ConnectionHandle,
         std::span<const std::byte> payload,
         mycore::net_transport::DeliveryMode delivery) override {
        sent_delivery.push_back(delivery);
        sent_payloads.emplace_back(payload.begin(), payload.end());
        return mycore::net_transport::SendStatus::Sent;
    }

    [[nodiscard]] bool disconnect(mycore::net_transport::ConnectionHandle) override {
        return true;
    }

    [[nodiscard]] std::optional<mycore::net_transport::TransportStatistics>
    statistics(mycore::net_transport::ConnectionHandle) const override {
        return std::nullopt;
    }

    std::vector<mycore::net_transport::Event> events;
    std::vector<mycore::net_transport::DeliveryMode> sent_delivery;
    std::vector<std::vector<std::byte>> sent_payloads;
};

[[nodiscard]] std::vector<std::byte> encode_bytes(const dots::protocol::Message& message) {
    auto result = dots::protocol::encode(message);
    auto* bytes = std::get_if<dots::protocol::EncodedMessage>(&result);
    REQUIRE(bytes != nullptr);
    return std::move(*bytes);
}

} // namespace

TEST_CASE("Two in-memory clients receive authoritative identities and snapshots",
          "[dots][session]") {
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{network.server_endpoint()};
    dots::client_runtime::Runtime first{network.connect_client()};
    dots::client_runtime::Runtime second{network.connect_client()};

    REQUIRE_FALSE(first.process_events().has_value());
    REQUIRE_FALSE(second.process_events().has_value());
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(first.process_events().has_value());
    REQUIRE_FALSE(second.process_events().has_value());

    REQUIRE(first.state() == dots::client_runtime::State::Ready);
    REQUIRE(second.state() == dots::client_runtime::State::Ready);
    REQUIRE(first.client_id() != second.client_id());
    REQUIRE(first.controlled_entity_id() != second.controlled_entity_id());
    REQUIRE(server.client_count() == 2);
    REQUIRE(server.world().player_count() == 2);
    REQUIRE(first.world().player_count() == 1);
    REQUIRE(second.world().player_count() == 2);
}

TEST_CASE("Authoritative input moves only the owning player and is acknowledged",
          "[dots][session]") {
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{network.server_endpoint()};
    dots::client_runtime::Runtime first{network.connect_client()};
    dots::client_runtime::Runtime second{network.connect_client()};
    REQUIRE_FALSE(first.process_events().has_value());
    REQUIRE_FALSE(second.process_events().has_value());
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(first.process_events().has_value());
    REQUIRE_FALSE(second.process_events().has_value());

    REQUIRE(first.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(server.step().has_value());
    REQUIRE_FALSE(server.step().has_value());
    REQUIRE_FALSE(first.process_events().has_value());
    REQUIRE_FALSE(second.process_events().has_value());

    const auto* first_entity = first.world().find(first.controlled_entity_id());
    const auto* second_entity = first.world().find(second.controlled_entity_id());
    REQUIRE(first_entity != nullptr);
    REQUIRE(second_entity != nullptr);
    CHECK(first_entity->position_x == Catch::Approx(0.4F));
    CHECK(second_entity->position_x == Catch::Approx(0.0F));
    CHECK(first.world().last_processed_input_id() == dots::protocol::InputSequenceId{0});
    CHECK_FALSE(second.world().last_processed_input_id().is_valid());
}

TEST_CASE("Disconnect cleanup removes only the owned authoritative player", "[dots][session]") {
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{network.server_endpoint()};
    dots::client_runtime::Runtime first{network.connect_client()};
    dots::client_runtime::Runtime second{network.connect_client()};
    REQUIRE_FALSE(first.process_events().has_value());
    REQUIRE_FALSE(second.process_events().has_value());
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(first.process_events().has_value());
    REQUIRE_FALSE(second.process_events().has_value());

    REQUIRE(first.disconnect());
    CHECK(first.state() == dots::client_runtime::State::Disconnected);
    REQUIRE_FALSE(server.process_events().has_value());
    CHECK(server.client_count() == 1);
    CHECK(server.world().player_count() == 1);
    CHECK(second.state() == dots::client_runtime::State::Ready);
}

TEST_CASE("Invalid packets disconnect one client without stopping another", "[dots][session]") {
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{network.server_endpoint()};
    dots::client_runtime::Runtime healthy{network.connect_client()};
    complete_handshake(healthy, server);

    auto& offender = network.connect_client();
    const auto events = offender.poll();
    REQUIRE(events.size() == 1);
    const auto connection = std::get<mycore::net_transport::Connected>(events.front()).connection;
    const std::array invalid{std::byte{0xFF}};
    REQUIRE(offender.send(connection, invalid, mycore::net_transport::DeliveryMode::Reliable) ==
            mycore::net_transport::SendStatus::Sent);

    REQUIRE_FALSE(server.process_events().has_value());
    CHECK(server.rejected_packet_count() == 1);
    CHECK(server.client_count() == 1);
    CHECK(server.world().player_count() == 1);
    REQUIRE_FALSE(server.step().has_value());
    CHECK(healthy.state() == dots::client_runtime::State::Ready);
}

TEST_CASE("Client accepts an initial snapshot before its welcome", "[dots][session]") {
    ManualEndpoint endpoint;
    const mycore::net_transport::ConnectionHandle connection{4};
    endpoint.events.push_back(mycore::net_transport::Connected{.connection = connection});
    dots::client_runtime::Runtime client{endpoint};
    REQUIRE_FALSE(client.process_events().has_value());
    REQUIRE(client.state() == dots::client_runtime::State::Handshaking);
    REQUIRE(endpoint.sent_delivery == std::vector{mycore::net_transport::DeliveryMode::Reliable});

    endpoint.events.push_back(mycore::net_transport::PayloadReceived{
        .connection = connection,
        .delivery = mycore::net_transport::DeliveryMode::Unreliable,
        .payload = encode_bytes(dots::protocol::FullSnapshot{
            .snapshot_id = dots::protocol::SnapshotId{0},
            .entities = {{
                .entity_id = dots::protocol::EntityId{8},
                .kind = dots::protocol::EntityKind::Player,
                .mass = 16.0F,
            }},
        }),
    });
    REQUIRE_FALSE(client.process_events().has_value());
    REQUIRE(client.state() == dots::client_runtime::State::Handshaking);

    endpoint.events.push_back(mycore::net_transport::PayloadReceived{
        .connection = connection,
        .delivery = mycore::net_transport::DeliveryMode::Reliable,
        .payload = encode_bytes(dots::protocol::ServerWelcome{
            .client_id = dots::protocol::ClientId{2},
            .controlled_entity_id = dots::protocol::EntityId{8},
        }),
    });
    REQUIRE_FALSE(client.process_events().has_value());
    REQUIRE(client.state() == dots::client_runtime::State::Ready);
}

TEST_CASE("Client reports accepted snapshot age and rolling receive rate",
          "[dots][session][statistics]") {
    using namespace std::chrono_literals;
    ManualEndpoint endpoint;
    const mycore::net_transport::ConnectionHandle connection{5};
    const auto base = std::chrono::steady_clock::time_point{10s};
    endpoint.events.push_back(mycore::net_transport::Connected{.connection = connection});
    dots::client_runtime::Runtime client{endpoint};
    REQUIRE_FALSE(client.process_events(base).has_value());

    endpoint.events.push_back(mycore::net_transport::PayloadReceived{
        .connection = connection,
        .delivery = mycore::net_transport::DeliveryMode::Unreliable,
        .payload = encode_bytes(dots::protocol::FullSnapshot{
            .snapshot_id = dots::protocol::SnapshotId{0},
        }),
    });
    REQUIRE_FALSE(client.process_events(base + 100ms).has_value());

    const auto recent = client.replication_statistics(base + 350ms);
    REQUIRE(recent.latest_snapshot_age == 250ms);
    CHECK(recent.accepted_snapshots_per_second == 1.0F);
    CHECK(recent.accepted_snapshot_count == 1);

    const auto old = client.replication_statistics(base + 2s);
    CHECK(old.accepted_snapshots_per_second == 0.0F);
    CHECK(old.accepted_snapshot_count == 1);
}

TEST_CASE("Dots handshake and snapshots work over native loopback", "[dots][session][native]") {
    using namespace std::chrono_literals;
    const auto bind = mycore::net_transport::NetworkAddress::parse("127.0.0.1:0");
    REQUIRE(bind.has_value());
    mycore::net_transport::GameNetworkingSocketsNetwork network;
    const auto listening = network.listen(*bind);
    auto& endpoint = network.connect(listening.address);
    dots::server::Runtime server{*listening.endpoint};
    dots::client_runtime::Runtime client{endpoint};

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (client.state() != dots::client_runtime::State::Ready &&
           std::chrono::steady_clock::now() < deadline) {
        REQUIRE_FALSE(client.process_events().has_value());
        REQUIRE_FALSE(server.process_events().has_value());
        REQUIRE_FALSE(client.process_events().has_value());
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(client.state() == dots::client_runtime::State::Ready);
    REQUIRE(server.client_count() == 1);

    REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    const auto input_deadline = std::chrono::steady_clock::now() + 100ms;
    while (std::chrono::steady_clock::now() < input_deadline) {
        REQUIRE_FALSE(server.process_events().has_value());
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE_FALSE(server.step().has_value());
    REQUIRE_FALSE(server.step().has_value());
    const auto snapshot_deadline = std::chrono::steady_clock::now() + 5s;
    while (client.world().server_tick() < 2 &&
           std::chrono::steady_clock::now() < snapshot_deadline) {
        REQUIRE_FALSE(client.process_events().has_value());
        std::this_thread::sleep_for(1ms);
    }
    CHECK(client.world().server_tick() == 2);
    CHECK(client.world().last_processed_input_id() == dots::protocol::InputSequenceId{0});

    REQUIRE(client.disconnect());
    CHECK(client.state() == dots::client_runtime::State::Disconnected);
    const auto disconnect_deadline = std::chrono::steady_clock::now() + 5s;
    while (server.client_count() != 0 && std::chrono::steady_clock::now() < disconnect_deadline) {
        REQUIRE_FALSE(server.process_events().has_value());
        std::this_thread::sleep_for(1ms);
    }
    CHECK(server.client_count() == 0);
}

TEST_CASE("Native Dots server rejects malformed handshake data", "[dots][session][native]") {
    using namespace std::chrono_literals;
    const auto bind = mycore::net_transport::NetworkAddress::parse("127.0.0.1:0");
    REQUIRE(bind.has_value());
    mycore::net_transport::GameNetworkingSocketsNetwork network;
    const auto listening = network.listen(*bind);
    auto& endpoint = network.connect(listening.address);
    dots::server::Runtime server{*listening.endpoint};

    mycore::net_transport::ConnectionHandle connection;
    const auto connection_deadline = std::chrono::steady_clock::now() + 5s;
    while (!connection.is_valid() && std::chrono::steady_clock::now() < connection_deadline) {
        REQUIRE_FALSE(server.process_events().has_value());
        for (const auto& event : endpoint.poll()) {
            if (const auto* connected = std::get_if<mycore::net_transport::Connected>(&event)) {
                connection = connected->connection;
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(connection.is_valid());

    const std::array malformed{std::byte{0xFF}};
    REQUIRE(endpoint.send(connection, malformed, mycore::net_transport::DeliveryMode::Reliable) ==
            mycore::net_transport::SendStatus::Sent);
    const auto rejection_deadline = std::chrono::steady_clock::now() + 5s;
    while (server.rejected_packet_count() == 0 &&
           std::chrono::steady_clock::now() < rejection_deadline) {
        REQUIRE_FALSE(server.process_events().has_value());
        static_cast<void>(endpoint.poll());
        std::this_thread::sleep_for(1ms);
    }
    CHECK(server.rejected_packet_count() == 1);
    CHECK(server.client_count() == 0);
}

#include "dots/client_runtime/client_runtime.hpp"
#include "dots/protocol/codec.hpp"
#include "dots/server/server_runtime.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <span>
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

#include "dots/client_runtime/client_runtime.hpp"
#include "dots/protocol/codec.hpp"
#include "dots/server/server_runtime.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <optional>
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
        if (fail_send_at && sent_payloads.size() == *fail_send_at) {
            return mycore::net_transport::SendStatus::QueueFull;
        }
        sent_delivery.push_back(delivery);
        sent_payloads.emplace_back(payload.begin(), payload.end());
        return mycore::net_transport::SendStatus::Sent;
    }

    [[nodiscard]] bool disconnect(mycore::net_transport::ConnectionHandle connection) override {
        disconnected_connections.push_back(connection);
        return true;
    }

    [[nodiscard]] std::optional<mycore::net_transport::TransportStatistics>
    statistics(mycore::net_transport::ConnectionHandle) const override {
        return std::nullopt;
    }

    std::vector<mycore::net_transport::Event> events;
    std::vector<mycore::net_transport::DeliveryMode> sent_delivery;
    std::vector<std::vector<std::byte>> sent_payloads;
    std::vector<mycore::net_transport::ConnectionHandle> disconnected_connections;
    std::optional<std::size_t> fail_send_at;
};

[[nodiscard]] std::vector<std::byte> encode_bytes(const dots::protocol::Message& message) {
    auto result = dots::protocol::encode(message);
    auto* bytes = std::get_if<dots::protocol::EncodedMessage>(&result);
    REQUIRE(bytes != nullptr);
    return std::move(*bytes);
}

[[nodiscard]] dots::protocol::Message decode_bytes(std::span<const std::byte> bytes) {
    auto result = dots::protocol::decode(bytes);
    auto* message = std::get_if<dots::protocol::Message>(&result);
    REQUIRE(message != nullptr);
    return std::move(*message);
}

[[nodiscard]] dots::protocol::InputSample
input_sample(std::uint32_t sequence_id, std::uint32_t client_tick, mycore::math::Vector2 movement) {
    return {
        .sequence_id = dots::protocol::InputSequenceId{sequence_id},
        .client_tick = client_tick,
        .movement_x = movement.x,
        .movement_y = movement.y,
    };
}

[[nodiscard]] dots::protocol::RecipientSessionState
playing_session(dots::protocol::EntityId primary_entity_id = dots::protocol::EntityId{8}) {
    return {
        .mode = dots::protocol::SessionMode::Playing,
        .owned_entity_ids = {primary_entity_id},
        .primary_entity_id = primary_entity_id,
    };
}

[[nodiscard]] dots::protocol::EntityState
player_state(dots::protocol::EntityId entity_id = dots::protocol::EntityId{8}) {
    return {
        .entity_id = entity_id,
        .kind = dots::protocol::EntityKind::Player,
        .owner_id = dots::protocol::PlayerOwnerId{3},
        .mass = 16.0F,
    };
}

void send_input_packet(mycore::net_transport::Endpoint& endpoint,
                       mycore::net_transport::ConnectionHandle connection,
                       std::vector<dots::protocol::InputSample> samples) {
    const auto bytes = encode_bytes(dots::protocol::InputPacket{
        .last_received_snapshot_id = dots::protocol::SnapshotId{0},
        .samples = std::move(samples),
    });
    REQUIRE(endpoint.send(connection, bytes, mycore::net_transport::DeliveryMode::Unreliable) ==
            mycore::net_transport::SendStatus::Sent);
}

[[nodiscard]] mycore::net_transport::ConnectionHandle
complete_raw_handshake(mycore::net_transport::Endpoint& client, dots::server::Runtime& server) {
    const auto connected_events = client.poll();
    REQUIRE(connected_events.size() == 1);
    const auto* connected = std::get_if<mycore::net_transport::Connected>(&connected_events[0]);
    REQUIRE(connected != nullptr);
    const auto hello = encode_bytes(dots::protocol::ClientHello{});
    REQUIRE(
        client.send(connected->connection, hello, mycore::net_transport::DeliveryMode::Reliable) ==
        mycore::net_transport::SendStatus::Sent);
    REQUIRE_FALSE(server.process_events().has_value());
    const auto handshake_messages = client.poll();
    REQUIRE(handshake_messages.size() == 2);
    return connected->connection;
}

void complete_manual_handshake(ManualEndpoint& endpoint,
                               dots::client_runtime::Runtime& client,
                               mycore::net_transport::ConnectionHandle connection) {
    endpoint.events.push_back(mycore::net_transport::Connected{.connection = connection});
    REQUIRE_FALSE(client.process_events().has_value());
    endpoint.events.push_back(mycore::net_transport::PayloadReceived{
        .connection = connection,
        .delivery = mycore::net_transport::DeliveryMode::Reliable,
        .payload = encode_bytes(dots::protocol::ServerWelcome{
            .client_id = dots::protocol::ClientId{2},
        }),
    });
    endpoint.events.push_back(mycore::net_transport::PayloadReceived{
        .connection = connection,
        .delivery = mycore::net_transport::DeliveryMode::Unreliable,
        .payload = encode_bytes(dots::protocol::FullSnapshot{
            .snapshot_id = dots::protocol::SnapshotId{0},
            .recipient = playing_session(),
            .entities = {player_state()},
        }),
    });
    REQUIRE_FALSE(client.process_events().has_value());
    REQUIRE(client.state() == dots::client_runtime::State::Ready);
    endpoint.sent_delivery.clear();
    endpoint.sent_payloads.clear();
}

[[nodiscard]] std::optional<dots::protocol::FullSnapshot>
find_snapshot(std::span<const mycore::net_transport::Event> events) {
    std::optional<dots::protocol::FullSnapshot> snapshot;
    for (const auto& event : events) {
        const auto* received = std::get_if<mycore::net_transport::PayloadReceived>(&event);
        if (received == nullptr) {
            continue;
        }
        auto message = decode_bytes(received->payload);
        if (auto* value = std::get_if<dots::protocol::FullSnapshot>(&message)) {
            snapshot = std::move(*value);
        }
    }
    return snapshot;
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
    CHECK(server.world().player_owner(server.world().player_ids()[0]) !=
          server.world().player_owner(server.world().player_ids()[1]));
    const auto* first_owned = first.world().find(first.primary_entity_id());
    const auto* second_owned = second.world().find(second.primary_entity_id());
    REQUIRE(first_owned != nullptr);
    REQUIRE(second_owned != nullptr);
    CHECK(first_owned->owner_id != second_owned->owner_id);
    const auto first_spawn = server.world().position(server.world().player_ids()[0]);
    const auto second_spawn = server.world().position(server.world().player_ids()[1]);
    REQUIRE(first_spawn.has_value());
    REQUIRE(second_spawn.has_value());
    CHECK(*first_spawn != *second_spawn);
    CHECK(*first_spawn == mycore::math::Vector2{});
    CHECK(*second_spawn != mycore::math::Vector2{});
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
    CHECK(second_entity->position_x == Catch::Approx(12.0F));
    CHECK(first.world().last_processed_input_id() == dots::protocol::InputSequenceId{0});
    CHECK_FALSE(second.world().last_processed_input_id().is_valid());
}

TEST_CASE("Respawn requests while playing are acknowledged and explicitly rejected",
          "[dots][session][lifecycle]") {
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{network.server_endpoint()};
    dots::client_runtime::Runtime client{network.connect_client()};
    complete_handshake(client, server);
    const auto original_player = client.primary_entity_id();

    REQUIRE(client.send_input(0, {}, dots::protocol::kRespawnActionBit) ==
            dots::client_runtime::InputSendResult::Sent);
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(server.step().has_value());
    REQUIRE_FALSE(server.step().has_value());
    REQUIRE_FALSE(client.process_events().error.has_value());

    CHECK(client.world().last_processed_input_id() == dots::protocol::InputSequenceId{0});
    CHECK(client.latest_respawn_request_id() == dots::protocol::InputSequenceId{0});
    CHECK(client.latest_respawn_result() == dots::protocol::RespawnResult::RejectedNotSpectating);
    CHECK(client.session_mode() == dots::protocol::SessionMode::Playing);
    CHECK(client.primary_entity_id() == original_player);
    CHECK(server.world().player_count() == 1);
}

TEST_CASE("Defeat state survives snapshot loss and respawn remains server-authoritative",
          "[dots][session][lifecycle]") {
    dots::simulation::World initial_world;
    REQUIRE(initial_world.spawn_food({}).has_value());
    mycore::net_transport::InMemoryNetwork network;
    auto& first_endpoint = network.connect_client();
    auto& second_endpoint = network.connect_client();
    dots::server::Runtime server{
        network.server_endpoint(),
        std::move(initial_world),
        {.respawn_cooldown_ticks = 4},
    };
    dots::client_runtime::Runtime first{first_endpoint};
    dots::client_runtime::Runtime second{second_endpoint};
    complete_handshake(first, server);
    complete_handshake(second, server);

    const auto first_player = first.primary_entity_id();
    const auto defeated_player = second.primary_entity_id();
    const auto* defeated_before = second.world().find(defeated_player);
    REQUIRE(defeated_before != nullptr);
    const auto defeated_owner = defeated_before->owner_id;
    CHECK(second.respawn_cooldown_ticks() == 4);

    std::uint32_t client_tick{};
    bool absorbed{};
    for (; client_tick < 32 && !absorbed; ++client_tick) {
        REQUIRE(first.send_input(client_tick, {1.0F, 0.0F}) ==
                dots::client_runtime::InputSendResult::Sent);
        REQUIRE(second.send_input(client_tick, {-1.0F, 0.0F}) ==
                dots::client_runtime::InputSendResult::Sent);
        REQUIRE_FALSE(server.process_events().has_value());
        REQUIRE_FALSE(server.step().has_value());
        REQUIRE_FALSE(first.process_events().error.has_value());
        absorbed = !server.world().last_step_events().empty();
        if (!absorbed) {
            REQUIRE_FALSE(second.process_events().error.has_value());
        }
    }
    REQUIRE(absorbed);

    if ((server.world().tick().value() % 2U) != 0U) {
        REQUIRE(first.send_input(client_tick, {1.0F, 0.0F}) ==
                dots::client_runtime::InputSendResult::Sent);
        REQUIRE(second.send_input(client_tick, {-1.0F, 0.0F}) ==
                dots::client_runtime::InputSendResult::Sent);
        ++client_tick;
        REQUIRE_FALSE(server.process_events().has_value());
        REQUIRE_FALSE(server.step().has_value());
        REQUIRE_FALSE(first.process_events().error.has_value());
    }

    const auto lost_transition = second_endpoint.poll();
    REQUIRE_FALSE(lost_transition.empty());
    CHECK(second.session_mode() == dots::protocol::SessionMode::Playing);
    CHECK(second.primary_entity_id() == defeated_player);

    while (second.session_mode() == dots::protocol::SessionMode::Playing) {
        REQUIRE(first.send_input(client_tick, {1.0F, 0.0F}) ==
                dots::client_runtime::InputSendResult::Sent);
        REQUIRE(second.send_input(client_tick, {-1.0F, 0.0F}) ==
                dots::client_runtime::InputSendResult::Sent);
        ++client_tick;
        REQUIRE_FALSE(server.process_events().has_value());
        REQUIRE_FALSE(server.step().has_value());
        REQUIRE_FALSE(first.process_events().error.has_value());
        REQUIRE_FALSE(second.process_events().error.has_value());
    }

    REQUIRE(second.session_mode() == dots::protocol::SessionMode::Spectating);
    CHECK(server.client_count() == 2);
    CHECK(server.world().player_count() == 1);
    CHECK(second.owned_entity_ids().empty());
    CHECK_FALSE(second.primary_entity_id().is_valid());
    CHECK(second.follow_entity_id() == first_player);
    REQUIRE(second.defeat_tick().has_value());
    REQUIRE(second.respawn_available_tick().has_value());
    CHECK(*second.respawn_available_tick() == *second.defeat_tick() + 4U);
    REQUIRE(second.latest_absorption().has_value());
    CHECK(second.latest_absorption()->absorber_entity_id == first_player);
    CHECK(second.latest_absorption()->victim_entity_id == defeated_player);
    CHECK(second.latest_absorption()->transferred_mass == Catch::Approx(16.0F));
    CHECK_FALSE(second.predicted_position().has_value());

    const auto early_request_tick = client_tick;
    REQUIRE(second.send_input(client_tick, {}, dots::protocol::kRespawnActionBit) ==
            dots::client_runtime::InputSendResult::Sent);
    REQUIRE(first.send_input(client_tick, {}) == dots::client_runtime::InputSendResult::Sent);
    ++client_tick;
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(server.step().has_value());
    REQUIRE_FALSE(first.process_events().error.has_value());
    REQUIRE_FALSE(second.process_events().error.has_value());

    while ((server.world().tick().value() % 2U) != 0U) {
        REQUIRE(second.send_input(client_tick, {}) == dots::client_runtime::InputSendResult::Sent);
        REQUIRE(first.send_input(client_tick, {}) == dots::client_runtime::InputSendResult::Sent);
        ++client_tick;
        REQUIRE_FALSE(server.process_events().has_value());
        REQUIRE_FALSE(server.step().has_value());
        REQUIRE_FALSE(first.process_events().error.has_value());
        REQUIRE_FALSE(second.process_events().error.has_value());
    }

    CHECK(second.session_mode() == dots::protocol::SessionMode::Spectating);
    CHECK(second.latest_respawn_request_id() ==
          dots::protocol::InputSequenceId{early_request_tick});
    CHECK(second.latest_respawn_result() == dots::protocol::RespawnResult::RejectedCooldown);
    CHECK(second.world().last_processed_input_id() >=
          dots::protocol::InputSequenceId{early_request_tick});
    CHECK(server.world().tick().value() >= *second.respawn_available_tick());
    CHECK(server.world().player_count() == 1);

    const auto accepted_request_tick = client_tick;
    REQUIRE(second.send_input(client_tick, {}, dots::protocol::kRespawnActionBit) ==
            dots::client_runtime::InputSendResult::Sent);
    REQUIRE(first.send_input(client_tick, {}) == dots::client_runtime::InputSendResult::Sent);
    ++client_tick;
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(server.step().has_value());
    REQUIRE_FALSE(first.process_events().error.has_value());
    REQUIRE_FALSE(second.process_events().error.has_value());

    while (second.session_mode() == dots::protocol::SessionMode::Spectating) {
        REQUIRE(second.send_input(client_tick, {}) == dots::client_runtime::InputSendResult::Sent);
        REQUIRE(first.send_input(client_tick, {}) == dots::client_runtime::InputSendResult::Sent);
        ++client_tick;
        REQUIRE_FALSE(server.process_events().has_value());
        REQUIRE_FALSE(server.step().has_value());
        REQUIRE_FALSE(first.process_events().error.has_value());
        REQUIRE_FALSE(second.process_events().error.has_value());
    }

    CHECK(second.session_mode() == dots::protocol::SessionMode::Playing);
    CHECK(second.latest_respawn_request_id() ==
          dots::protocol::InputSequenceId{accepted_request_tick});
    CHECK(second.latest_respawn_result() == dots::protocol::RespawnResult::Accepted);
    CHECK(second.owned_entity_ids().size() == 1);
    CHECK(second.primary_entity_id() != defeated_player);
    CHECK_FALSE(second.follow_entity_id().is_valid());
    CHECK_FALSE(second.defeat_tick().has_value());
    CHECK_FALSE(second.respawn_available_tick().has_value());
    REQUIRE(second.predicted_position().has_value());
    const auto* respawned = second.world().find(second.primary_entity_id());
    REQUIRE(respawned != nullptr);
    CHECK(respawned->owner_id == defeated_owner);
    CHECK(server.client_count() == 2);
    CHECK(server.world().player_count() == 2);
}

TEST_CASE("Disconnecting a spectator preserves unrelated authoritative players",
          "[dots][session][lifecycle]") {
    dots::simulation::World initial_world;
    const auto survivor = initial_world.spawn_player(dots::simulation::PlayerOwnerId{99}, {});
    REQUIRE(survivor.has_value());
    REQUIRE(initial_world.spawn_food({}).has_value());
    REQUIRE(initial_world.step());
    REQUIRE(initial_world.mass(*survivor) == 17.0F);

    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{network.server_endpoint(), std::move(initial_world)};
    dots::client_runtime::Runtime client{network.connect_client()};
    complete_handshake(client, server);

    for (std::uint32_t tick = 0;
         tick < 32 && client.session_mode() != dots::protocol::SessionMode::Spectating;
         ++tick) {
        REQUIRE(client.send_input(tick, {-1.0F, 0.0F}) ==
                dots::client_runtime::InputSendResult::Sent);
        REQUIRE_FALSE(server.process_events().has_value());
        REQUIRE_FALSE(server.step().has_value());
        REQUIRE_FALSE(client.process_events().error.has_value());
    }

    REQUIRE(client.session_mode() == dots::protocol::SessionMode::Spectating);
    CHECK(server.client_count() == 1);
    CHECK(server.world().player_count() == 1);
    REQUIRE(client.disconnect());
    REQUIRE_FALSE(server.process_events().has_value());
    CHECK(server.client_count() == 0);
    CHECK(server.world().player_count() == 1);
    CHECK(server.world().contains(*survivor));
}

TEST_CASE("Client input redundancy includes only bounded unacknowledged samples",
          "[dots][session][input][redundancy]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{7};
    complete_manual_handshake(endpoint, client, connection);

    REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE(client.send_input(1, {0.0F, 1.0F}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE(client.send_input(2, {-1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE(client.send_input(3, {0.0F, -1.0F}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE(endpoint.sent_payloads.size() == 4);

    const std::array expected_counts{1U, 2U, 3U, 3U};
    const std::array expected_first_sequences{0U, 0U, 0U, 1U};
    for (std::size_t index = 0; index < endpoint.sent_payloads.size(); ++index) {
        auto message = decode_bytes(endpoint.sent_payloads[index]);
        const auto* packet = std::get_if<dots::protocol::InputPacket>(&message);
        REQUIRE(packet != nullptr);
        REQUIRE(packet->samples.size() == expected_counts[index]);
        CHECK(packet->samples.front().sequence_id ==
              dots::protocol::InputSequenceId{expected_first_sequences[index]});
        CHECK(packet->samples.back().sequence_id ==
              dots::protocol::InputSequenceId{static_cast<std::uint32_t>(index)});
    }

    endpoint.events.push_back(mycore::net_transport::PayloadReceived{
        .connection = connection,
        .delivery = mycore::net_transport::DeliveryMode::Unreliable,
        .payload = encode_bytes(dots::protocol::FullSnapshot{
            .snapshot_id = dots::protocol::SnapshotId{1},
            .last_processed_input_id = dots::protocol::InputSequenceId{3},
            .recipient = playing_session(),
            .entities = {player_state()},
        }),
    });
    REQUIRE_FALSE(client.process_events().has_value());
    REQUIRE(client.send_input(4, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    auto message = decode_bytes(endpoint.sent_payloads.back());
    const auto* packet = std::get_if<dots::protocol::InputPacket>(&message);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->samples.size() == 1);
    CHECK(packet->samples.front().sequence_id == dots::protocol::InputSequenceId{4});
}

TEST_CASE("Client can disable input redundancy", "[dots][session][input][redundancy]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint, {.input_redundancy = false}};
    complete_manual_handshake(endpoint, client, mycore::net_transport::ConnectionHandle{8});

    REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE(client.send_input(1, {0.0F, 1.0F}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE(endpoint.sent_payloads.size() == 2);
    for (const auto& bytes : endpoint.sent_payloads) {
        auto message = decode_bytes(bytes);
        const auto* packet = std::get_if<dots::protocol::InputPacket>(&message);
        REQUIRE(packet != nullptr);
        CHECK(packet->samples.size() == 1);
    }
}

TEST_CASE("Server queues reordered redundant inputs and consumes one per tick",
          "[dots][session][input][queue]") {
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{network.server_endpoint()};
    auto& client = network.connect_client();
    const auto connection = complete_raw_handshake(client, server);

    const auto right = input_sample(0, 0, {1.0F, 0.0F});
    const auto up = input_sample(1, 1, {0.0F, 1.0F});
    const auto left = input_sample(2, 2, {-1.0F, 0.0F});
    send_input_packet(client, connection, {left});
    send_input_packet(client, connection, {right, up, left});
    REQUIRE_FALSE(server.process_events().has_value());

    REQUIRE(server.world().player_ids().size() == 1);
    const auto player = server.world().player_ids().front();
    const auto spawn_position = server.world().position(player);
    REQUIRE(spawn_position.has_value());
    REQUIRE_FALSE(server.step().has_value());
    REQUIRE(server.world().position(player).has_value());
    CHECK(server.world().position(player)->x == Catch::Approx(spawn_position->x + 0.2F));
    CHECK(server.world().position(player)->y == Catch::Approx(spawn_position->y));

    REQUIRE_FALSE(server.step().has_value());
    CHECK(server.world().position(player)->x == Catch::Approx(spawn_position->x + 0.2F));
    CHECK(server.world().position(player)->y == Catch::Approx(spawn_position->y + 0.2F));
    const auto queued_snapshot = find_snapshot(client.poll());
    REQUIRE(queued_snapshot.has_value());
    CHECK(queued_snapshot->last_processed_input_id == dots::protocol::InputSequenceId{1});
    CHECK(queued_snapshot->pending_input_count == 1);

    REQUIRE_FALSE(server.step().has_value());
    CHECK(server.world().position(player)->x == Catch::Approx(spawn_position->x));
    CHECK(server.world().position(player)->y == Catch::Approx(spawn_position->y + 0.2F));
    REQUIRE_FALSE(server.step().has_value());
    CHECK(server.world().position(player)->x == Catch::Approx(spawn_position->x - 0.2F));
    CHECK(server.world().position(player)->y == Catch::Approx(spawn_position->y + 0.2F));
    const auto drained_snapshot = find_snapshot(client.poll());
    REQUIRE(drained_snapshot.has_value());
    CHECK(drained_snapshot->last_processed_input_id == dots::protocol::InputSequenceId{2});
    CHECK(drained_snapshot->pending_input_count == 0);
}

TEST_CASE("Server input queue overflow disconnects only the offending session",
          "[dots][session][input][queue]") {
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{network.server_endpoint()};
    dots::client_runtime::Runtime healthy{network.connect_client()};
    dots::client_runtime::Runtime offender{network.connect_client()};
    REQUIRE_FALSE(healthy.process_events().has_value());
    REQUIRE_FALSE(offender.process_events().has_value());
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(healthy.process_events().has_value());
    REQUIRE_FALSE(offender.process_events().has_value());

    for (std::uint32_t tick = 0; tick <= dots::protocol::kMaximumPendingInputCount; ++tick) {
        REQUIRE(offender.send_input(tick, {1.0F, 0.0F}) ==
                dots::client_runtime::InputSendResult::Sent);
    }
    REQUIRE_FALSE(server.process_events().has_value());
    CHECK(server.rejected_packet_count() == 1);
    CHECK(server.client_count() == 1);
    CHECK(server.world().player_count() == 1);
    CHECK(healthy.state() == dots::client_runtime::State::Ready);
    const auto healthy_spawn = server.world().position(server.world().player_ids().front());
    REQUIRE(healthy_spawn.has_value());

    REQUIRE(healthy.send_input(0, {0.0F, 1.0F}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(server.step().has_value());
    REQUIRE(server.world().player_ids().size() == 1);
    const auto position = server.world().position(server.world().player_ids().front());
    REQUIRE(position.has_value());
    CHECK(position->x == Catch::Approx(healthy_spawn->x));
    CHECK(position->y == Catch::Approx(healthy_spawn->y + 0.2F));
}

TEST_CASE("Server rejects conflicting data for a queued input sequence",
          "[dots][session][input][queue]") {
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{network.server_endpoint()};
    auto& client = network.connect_client();
    const auto connection = complete_raw_handshake(client, server);

    send_input_packet(client, connection, {input_sample(0, 0, {1.0F, 0.0F})});
    REQUIRE_FALSE(server.process_events().has_value());
    send_input_packet(client, connection, {input_sample(0, 0, {-1.0F, 0.0F})});
    REQUIRE_FALSE(server.process_events().has_value());

    CHECK(server.rejected_packet_count() == 1);
    CHECK(server.client_count() == 0);
    CHECK(server.world().player_count() == 0);
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

TEST_CASE("Server replicates a disconnected player removal to remaining clients",
          "[dots][session]") {
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{network.server_endpoint()};
    dots::client_runtime::Runtime observer{network.connect_client()};
    dots::client_runtime::Runtime departing{network.connect_client()};
    complete_handshake(observer, server);
    complete_handshake(departing, server);

    const auto departing_entity_id = departing.controlled_entity_id();
    REQUIRE_FALSE(server.step().has_value());
    REQUIRE_FALSE(server.step().has_value());
    const auto before_disconnect = observer.process_events();
    REQUIRE_FALSE(before_disconnect.error.has_value());
    REQUIRE(observer.world().find(departing_entity_id) != nullptr);

    REQUIRE(departing.disconnect());
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(server.step().has_value());
    REQUIRE_FALSE(server.step().has_value());

    const auto after_disconnect = observer.process_events();
    REQUIRE_FALSE(after_disconnect.error.has_value());
    REQUIRE_FALSE(after_disconnect.accepted_snapshots.empty());
    const auto& removal_snapshot = after_disconnect.accepted_snapshots.back().snapshot;
    CHECK(std::none_of(removal_snapshot.entities.begin(),
                       removal_snapshot.entities.end(),
                       [departing_entity_id](const auto& entity) {
                           return entity.entity_id == departing_entity_id;
                       }));
    CHECK(observer.world().find(departing_entity_id) == nullptr);
}

TEST_CASE("Server disconnects a client that exceeds its liveness timeout", "[dots][session]") {
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{
        network.server_endpoint(),
        {},
        {.liveness_timeout_ticks = 2},
    };
    dots::client_runtime::Runtime active{network.connect_client()};
    dots::client_runtime::Runtime inactive{network.connect_client()};
    complete_handshake(active, server);
    complete_handshake(inactive, server);

    REQUIRE(active.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(server.step().has_value());

    REQUIRE(active.send_input(1, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(server.step().has_value());

    REQUIRE_FALSE(server.step().has_value());
    CHECK(server.client_count() == 1);
    CHECK(server.world().player_count() == 1);
    CHECK(active.state() == dots::client_runtime::State::Ready);

    REQUIRE_FALSE(inactive.process_events().has_value());
    CHECK(inactive.state() == dots::client_runtime::State::Disconnected);
}

TEST_CASE("Server disconnects a transport connection that never handshakes", "[dots][session]") {
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{
        network.server_endpoint(),
        {},
        {
            .handshake_timeout_ticks = 2,
        },
    };
    auto& client = network.connect_client();
    const auto connected_events = client.poll();
    REQUIRE(connected_events.size() == 1);
    REQUIRE(std::holds_alternative<mycore::net_transport::Connected>(connected_events.front()));
    REQUIRE_FALSE(server.process_events().has_value());

    REQUIRE_FALSE(server.step().has_value());
    REQUIRE_FALSE(server.step().has_value());
    REQUIRE(client.poll().empty());
    REQUIRE_FALSE(server.step().has_value());

    const auto disconnected_events = client.poll();
    REQUIRE(disconnected_events.size() == 1);
    const auto* disconnected =
        std::get_if<mycore::net_transport::Disconnected>(&disconnected_events.front());
    REQUIRE(disconnected != nullptr);
    CHECK(disconnected->reason == mycore::net_transport::DisconnectReason::RemoteRequest);
}

TEST_CASE("Successful handshake starts the ready-session liveness window", "[dots][session]") {
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{
        network.server_endpoint(),
        {},
        {
            .liveness_timeout_ticks = 2,
            .handshake_timeout_ticks = 10,
        },
    };
    auto& client = network.connect_client();
    const auto connected_events = client.poll();
    REQUIRE(connected_events.size() == 1);
    const auto connection =
        std::get<mycore::net_transport::Connected>(connected_events.front()).connection;
    REQUIRE_FALSE(server.process_events().has_value());

    for (std::size_t tick = 0; tick < 5; ++tick) {
        REQUIRE_FALSE(server.step().has_value());
    }
    const auto hello = encode_bytes(dots::protocol::ClientHello{});
    REQUIRE(client.send(connection, hello, mycore::net_transport::DeliveryMode::Reliable) ==
            mycore::net_transport::SendStatus::Sent);
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE(server.client_count() == 1);

    REQUIRE_FALSE(server.step().has_value());
    CHECK(server.client_count() == 1);
    CHECK(server.world().player_count() == 1);
}

TEST_CASE("Server stops held movement after its missing-input window", "[dots][session]") {
    mycore::net_transport::InMemoryNetwork network;
    dots::server::Runtime server{
        network.server_endpoint(),
        {},
        {
            .liveness_timeout_ticks = 100,
            .input_hold_ticks = 2,
        },
    };
    dots::client_runtime::Runtime client{network.connect_client()};
    complete_handshake(client, server);

    const auto player_id = dots::simulation::EntityId{client.controlled_entity_id().value()};
    const auto spawn = server.world().position(player_id);
    REQUIRE(spawn.has_value());
    REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE_FALSE(server.process_events().has_value());
    REQUIRE_FALSE(server.step().has_value());

    REQUIRE_FALSE(server.step().has_value());
    REQUIRE_FALSE(server.step().has_value());
    const auto held_position = server.world().position(player_id);
    REQUIRE(held_position.has_value());
    CHECK(held_position->x == Catch::Approx(spawn->x + 0.6F));

    REQUIRE_FALSE(server.step().has_value());
    const auto stopped_position = server.world().position(player_id);
    REQUIRE(stopped_position.has_value());
    CHECK(stopped_position->x == Catch::Approx(held_position->x));
    CHECK(server.client_count() == 1);
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

TEST_CASE("Server closes a session whose handshake send fails", "[dots][session][send]") {
    ManualEndpoint endpoint;
    dots::server::Runtime server{endpoint};
    const mycore::net_transport::ConnectionHandle connection{9};
    endpoint.events.push_back(mycore::net_transport::Connected{.connection = connection});
    REQUIRE_FALSE(server.process_events().has_value());

    endpoint.fail_send_at = 1;
    endpoint.events.push_back(mycore::net_transport::PayloadReceived{
        .connection = connection,
        .delivery = mycore::net_transport::DeliveryMode::Reliable,
        .payload = encode_bytes(dots::protocol::ClientHello{}),
    });
    REQUIRE_FALSE(server.process_events().has_value());

    CHECK(server.client_count() == 0);
    CHECK(server.world().player_count() == 0);
    CHECK(endpoint.disconnected_connections == std::vector{connection});
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
            .recipient = playing_session(),
            .entities = {player_state()},
        }),
    });
    REQUIRE_FALSE(client.process_events().has_value());
    REQUIRE(client.state() == dots::client_runtime::State::Handshaking);

    endpoint.events.push_back(mycore::net_transport::PayloadReceived{
        .connection = connection,
        .delivery = mycore::net_transport::DeliveryMode::Reliable,
        .payload = encode_bytes(dots::protocol::ServerWelcome{
            .client_id = dots::protocol::ClientId{2},
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
            .recipient = playing_session(),
            .entities = {player_state()},
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

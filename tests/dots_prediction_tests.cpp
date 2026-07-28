#include "dots/client_runtime/client_runtime.hpp"
#include "dots/prediction/model.hpp"
#include "dots/protocol/codec.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr dots::protocol::EntityId kControlledEntity{8};
constexpr dots::protocol::PlayerOwnerId kControlledOwner{3};

[[nodiscard]] constexpr std::chrono::steady_clock::time_point
clock_time(std::chrono::steady_clock::duration offset) noexcept {
    return std::chrono::steady_clock::time_point{offset};
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
        if (fail_next_send) {
            fail_next_send = false;
            return mycore::net_transport::SendStatus::QueueFull;
        }
        sent_delivery.push_back(delivery);
        sent_payloads.emplace_back(payload.begin(), payload.end());
        return mycore::net_transport::SendStatus::Sent;
    }

    [[nodiscard]] bool disconnect(mycore::net_transport::ConnectionHandle) override {
        disconnect_called = true;
        return true;
    }

    [[nodiscard]] std::optional<mycore::net_transport::TransportStatistics>
    statistics(mycore::net_transport::ConnectionHandle) const override {
        return std::nullopt;
    }

    std::vector<mycore::net_transport::Event> events;
    std::vector<mycore::net_transport::DeliveryMode> sent_delivery;
    std::vector<std::vector<std::byte>> sent_payloads;
    bool fail_next_send{};
    bool disconnect_called{};
};

[[nodiscard]] dots::protocol::WorldRules world_rules() {
    return dots::replication::to_protocol(dots::simulation::WorldRules{});
}

void complete_protocol_fixture(dots::protocol::Message& message) {
    if (auto* welcome = std::get_if<dots::protocol::ServerWelcome>(&message)) {
        if (welcome->world_rules.initial_player_mass == 0.0F) {
            welcome->world_rules = world_rules();
        }
        return;
    }
    auto* value = std::get_if<dots::protocol::FullSnapshot>(&message);
    if (value == nullptr) {
        return;
    }
    for (const auto& entity : value->entities) {
        if (entity.kind != dots::protocol::EntityKind::Player ||
            std::any_of(value->owners.begin(),
                        value->owners.end(),
                        [&entity](const dots::protocol::OwnerState& owner) {
                            return owner.owner_id == entity.owner_id;
                        })) {
            continue;
        }
        value->owners.push_back({.owner_id = entity.owner_id});
    }
    std::sort(value->owners.begin(),
              value->owners.end(),
              [](const dots::protocol::OwnerState& lhs, const dots::protocol::OwnerState& rhs) {
                  return lhs.owner_id < rhs.owner_id;
              });
    auto next_entity_id = std::uint32_t{};
    for (const auto& entity : value->entities) {
        next_entity_id = std::max(next_entity_id, entity.entity_id.value() + 1U);
    }
    value->next_entity_id = dots::protocol::EntityId{next_entity_id};
    dots::simulation::WorldCheckpoint checkpoint{
        .rules = dots::simulation::WorldRules{},
        .tick = mycore::time::Tick{value->server_tick},
        .next_entity_id = next_entity_id,
        .owners = {},
        .players = {},
        .food = {},
    };
    for (const auto& owner : value->owners) {
        checkpoint.owners.push_back({
            .owner_id = dots::simulation::PlayerOwnerId{owner.owner_id.value()},
            .player_ids = {},
            .movement = {owner.movement_x, owner.movement_y},
            .last_non_zero_movement = {owner.last_non_zero_movement_x,
                                       owner.last_non_zero_movement_y},
            .last_input_id = dots::simulation::InputCommandId{owner.last_input_id.value()},
            .split_cooldown_end_tick = mycore::time::Tick{owner.split_cooldown_end_tick},
        });
    }
    for (const auto& entity : value->entities) {
        if (entity.kind == dots::protocol::EntityKind::Food) {
            checkpoint.food.push_back({
                .entity_id = dots::simulation::EntityId{entity.entity_id.value()},
                .position = {entity.position_x, entity.position_y},
            });
            continue;
        }
        std::optional<dots::simulation::PredictionKey> prediction_key;
        if (entity.prediction_key) {
            prediction_key = dots::simulation::PredictionKey{
                .owner_id =
                    dots::simulation::PlayerOwnerId{entity.prediction_key->owner_id.value()},
                .input_id =
                    dots::simulation::InputCommandId{entity.prediction_key->input_id.value()},
                .child_ordinal = entity.prediction_key->child_ordinal,
            };
        }
        checkpoint.players.push_back({
            .entity_id = dots::simulation::EntityId{entity.entity_id.value()},
            .owner_id = dots::simulation::PlayerOwnerId{entity.owner_id.value()},
            .position = {entity.position_x, entity.position_y},
            .mass = entity.mass,
            .launch_velocity = {entity.launch_velocity_x, entity.launch_velocity_y},
            .merge_eligible_tick = mycore::time::Tick{entity.merge_eligible_tick},
            .prediction_key = prediction_key,
        });
        const auto owner =
            std::find_if(checkpoint.owners.begin(),
                         checkpoint.owners.end(),
                         [&entity](const dots::simulation::OwnerCheckpoint& candidate) {
                             return candidate.owner_id.value() == entity.owner_id.value();
                         });
        REQUIRE(owner != checkpoint.owners.end());
        owner->player_ids.push_back(dots::simulation::EntityId{entity.entity_id.value()});
    }
    value->checkpoint_digest = dots::prediction::checkpoint_digest(checkpoint).value;
}

[[nodiscard]] std::vector<std::byte> encode_finalized(const dots::protocol::Message& message) {
    auto result = dots::protocol::encode(message);
    auto* bytes = std::get_if<dots::protocol::EncodedMessage>(&result);
    REQUIRE(bytes != nullptr);
    return std::move(*bytes);
}

[[nodiscard]] std::vector<std::byte> encode_bytes(dots::protocol::Message message) {
    complete_protocol_fixture(message);
    return encode_finalized(message);
}

[[nodiscard]] dots::protocol::Message decode_bytes(std::span<const std::byte> bytes) {
    auto result = dots::protocol::decode(bytes);
    auto* message = std::get_if<dots::protocol::Message>(&result);
    REQUIRE(message != nullptr);
    return std::move(*message);
}

[[nodiscard]] dots::protocol::FullSnapshot snapshot(std::uint32_t snapshot_id,
                                                    std::uint32_t server_tick,
                                                    dots::protocol::InputSequenceId acknowledgement,
                                                    mycore::math::Vector2 controlled_position,
                                                    std::uint8_t pending_input_count = 0) {
    return {
        .snapshot_id = dots::protocol::SnapshotId{snapshot_id},
        .server_tick = server_tick,
        .last_processed_input_id = acknowledgement,
        .pending_input_count = pending_input_count,
        .recipient =
            {
                .mode = dots::protocol::SessionMode::Playing,
                .owned_entity_ids = {kControlledEntity},
                .primary_entity_id = kControlledEntity,
            },
        .entities = {{
            .entity_id = kControlledEntity,
            .kind = dots::protocol::EntityKind::Player,
            .owner_id = kControlledOwner,
            .position_x = controlled_position.x,
            .position_y = controlled_position.y,
            .mass = 16.0F,
        }},
    };
}

void push_snapshot(ManualEndpoint& endpoint,
                   mycore::net_transport::ConnectionHandle connection,
                   const dots::protocol::FullSnapshot& value) {
    endpoint.events.push_back(mycore::net_transport::PayloadReceived{
        .connection = connection,
        .delivery = mycore::net_transport::DeliveryMode::Unreliable,
        .payload = encode_bytes(value),
    });
}

void complete_handshake(ManualEndpoint& endpoint,
                        dots::client_runtime::Runtime& client,
                        mycore::net_transport::ConnectionHandle connection) {
    endpoint.events.push_back(mycore::net_transport::Connected{.connection = connection});
    REQUIRE_FALSE(client.process_events(clock_time(10s)).has_value());
    endpoint.events.push_back(mycore::net_transport::PayloadReceived{
        .connection = connection,
        .delivery = mycore::net_transport::DeliveryMode::Reliable,
        .payload = encode_bytes(dots::protocol::ServerWelcome{
            .client_id = dots::protocol::ClientId{2},
            .respawn_cooldown_ticks = 90,
        }),
    });
    push_snapshot(
        endpoint, connection, snapshot(0, 0, dots::protocol::InputSequenceId::invalid(), {}));
    REQUIRE_FALSE(client.process_events(clock_time(10s)).has_value());
    REQUIRE(client.state() == dots::client_runtime::State::Ready);
    REQUIRE(client.predicted_position() == mycore::math::Vector2{});
    endpoint.sent_delivery.clear();
    endpoint.sent_payloads.clear();
}

void check_position(std::optional<mycore::math::Vector2> position, float x, float y) {
    REQUIRE(position.has_value());
    CHECK(position->x == Catch::Approx(x));
    CHECK(position->y == Catch::Approx(y));
}

} // namespace

TEST_CASE("Client prediction advances only successfully sent input", "[dots][prediction][send]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{10};
    complete_handshake(endpoint, client, connection);

    REQUIRE(client.send_input(0, {0.5F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    check_position(client.predicted_position(), 0.2F, 0.0F);
    const auto* replicated = client.world().find(kControlledEntity);
    REQUIRE(replicated != nullptr);
    CHECK(replicated->position_x == Catch::Approx(0.0F));

    auto statistics = client.prediction_statistics(clock_time(10s));
    CHECK(statistics.last_input_sent == dots::protocol::InputSequenceId{0});
    CHECK_FALSE(statistics.last_input_acknowledged.is_valid());
    CHECK(statistics.unacknowledged_input_count == 1);
    CHECK(statistics.history_count == 1);
    CHECK(statistics.history_capacity == dots::client_runtime::kPredictionHistoryCapacity);
    CHECK(statistics.history_high_water_mark == 1);

    REQUIRE(client.send_input(2, {0.0F, 1.0F}) ==
            dots::client_runtime::InputSendResult::InvalidClientTick);
    check_position(client.predicted_position(), 0.2F, 0.0F);
    CHECK(client.prediction_statistics(clock_time(10s)).history_count == 1);

    REQUIRE(client.send_input(1, {}, dots::protocol::kKnownInputActionBits << 1U) ==
            dots::client_runtime::InputSendResult::InvalidAction);

    REQUIRE(client.send_input(1, {std::numeric_limits<float>::infinity(), 0.0F}) ==
            dots::client_runtime::InputSendResult::InvalidMovement);
    check_position(client.predicted_position(), 0.2F, 0.0F);
    CHECK(client.prediction_statistics(clock_time(10s)).history_count == 1);
}

TEST_CASE("Client returns every accepted snapshot from one poll in delivery order",
          "[dots][prediction][replication]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{10};
    complete_handshake(endpoint, client, connection);

    push_snapshot(endpoint,
                  connection,
                  snapshot(1, 2, dots::protocol::InputSequenceId::invalid(), {0.2F, 0.0F}));
    push_snapshot(endpoint,
                  connection,
                  snapshot(2, 4, dots::protocol::InputSequenceId::invalid(), {0.4F, 0.0F}));

    const auto result = client.process_events(clock_time(11s));
    REQUIRE_FALSE(result.error.has_value());
    REQUIRE(result.accepted_snapshots.size() == 2);
    CHECK(result.accepted_snapshots[0].snapshot.snapshot_id == dots::protocol::SnapshotId{1});
    CHECK(result.accepted_snapshots[1].snapshot.snapshot_id == dots::protocol::SnapshotId{2});
    CHECK(result.accepted_snapshots[0].arrival_time == clock_time(11s));
    CHECK(client.world().snapshot_id() == dots::protocol::SnapshotId{2});
}

TEST_CASE("Transport send failure does not advance prediction or history",
          "[dots][prediction][send]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    complete_handshake(endpoint, client, mycore::net_transport::ConnectionHandle{11});
    endpoint.fail_next_send = true;

    REQUIRE(client.send_input(0, {1.0F, 0.0F}) ==
            dots::client_runtime::InputSendResult::TransportFailure);
    REQUIRE(client.state() == dots::client_runtime::State::Disconnected);
    check_position(client.predicted_position(), 0.0F, 0.0F);
    const auto statistics = client.prediction_statistics(clock_time(10s));
    CHECK_FALSE(statistics.last_input_sent.is_valid());
    CHECK(statistics.history_count == 0);
    CHECK(endpoint.disconnect_called);
}

TEST_CASE("Matching reconciliation discards acknowledged input and replays the remainder",
          "[dots][prediction][reconciliation]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{12};
    complete_handshake(endpoint, client, connection);
    REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE(client.send_input(1, {0.0F, 1.0F}) == dots::client_runtime::InputSendResult::Sent);
    check_position(client.predicted_position(), 0.2F, 0.2F);

    push_snapshot(
        endpoint, connection, snapshot(1, 2, dots::protocol::InputSequenceId{0}, {0.2F, 0.0F}, 7));
    REQUIRE_FALSE(client.process_events(clock_time(11s)).has_value());

    const auto* authority = client.world().find(kControlledEntity);
    REQUIRE(authority != nullptr);
    CHECK(authority->position_x == Catch::Approx(0.2F));
    CHECK(authority->position_y == Catch::Approx(0.0F));
    check_position(client.predicted_position(), 0.2F, 0.2F);
    REQUIRE(client.latest_replay_path().size() == 1);
    CHECK(client.latest_replay_path().front().x == Catch::Approx(0.2F));
    CHECK(client.latest_replay_path().front().y == Catch::Approx(0.2F));

    const auto statistics = client.prediction_statistics(clock_time(11s));
    CHECK(statistics.last_input_acknowledged == dots::protocol::InputSequenceId{0});
    CHECK(statistics.unacknowledged_input_count == 1);
    CHECK(statistics.history_count == 1);
    CHECK(statistics.latest_server_pending_input_count == 7);
    CHECK(statistics.server_pending_input_high_water_mark == 7);
    CHECK(statistics.rollback_snapshot_id == dots::protocol::SnapshotId{1});
    CHECK(statistics.rollback_server_tick == 2);
    CHECK(statistics.rollback_input_acknowledgement == dots::protocol::InputSequenceId{0});
    CHECK(statistics.latest_replay_count == 1);
    CHECK(statistics.total_replayed_input_count == 1);
    CHECK(statistics.maximum_replay_count == 1);
    CHECK(statistics.reconciliation_count == 1);
    CHECK(statistics.nonzero_correction_count == 0);
    CHECK(statistics.latest_correction_distance == Catch::Approx(0.0F));
    CHECK(statistics.latest_replay_milliseconds >= 0.0);
    CHECK(statistics.average_replay_milliseconds >= 0.0);
    CHECK(statistics.maximum_replay_milliseconds >= statistics.latest_replay_milliseconds);
}

TEST_CASE("Prediction remains immediate across a deterministic 200 ms authority delay",
          "[dots][prediction][reconciliation][impairment]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{22};
    const auto base = clock_time(10s);
    complete_handshake(endpoint, client, connection);

    // Six 30 Hz input ticks represent 200 ms without a newer authoritative snapshot.
    for (std::uint32_t tick = 0; tick < 6; ++tick) {
        REQUIRE(client.send_input(tick, {1.0F, 0.0F}) ==
                dots::client_runtime::InputSendResult::Sent);
    }
    check_position(client.predicted_position(), 1.2F, 0.0F);
    const auto* stale_authority = client.world().find(kControlledEntity);
    REQUIRE(stale_authority != nullptr);
    CHECK(stale_authority->position_x == Catch::Approx(0.0F));

    // Authority has consumed the first three samples. Replaying the remaining suffix must
    // reproduce the already-visible prediction without generating a correction.
    push_snapshot(
        endpoint, connection, snapshot(1, 3, dots::protocol::InputSequenceId{2}, {0.6F, 0.0F}));
    REQUIRE_FALSE(client.process_events(base + 200ms).has_value());

    check_position(client.predicted_position(), 1.2F, 0.0F);
    const auto statistics = client.prediction_statistics(base + 200ms);
    CHECK(statistics.last_input_acknowledged == dots::protocol::InputSequenceId{2});
    CHECK(statistics.unacknowledged_input_count == 3);
    CHECK(statistics.history_count == 3);
    CHECK(statistics.latest_replay_count == 3);
    CHECK(statistics.nonzero_correction_count == 0);
    CHECK(statistics.latest_correction_distance == Catch::Approx(0.0F));
}

TEST_CASE("Predicted identity remaps a split child to its authoritative entity",
          "[dots][prediction][identity][split]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{25};
    complete_handshake(endpoint, client, connection);

    REQUIRE(client.send_input(0, {}, dots::protocol::kSplitActionBit) ==
            dots::client_runtime::InputSendResult::Sent);
    REQUIRE(client.predicted_owned_entity_ids().size() == 2);
    CHECK(client.predicted_owned_entity_ids()[1] == dots::protocol::EntityId{9});

    auto authority = snapshot(1, 1, dots::protocol::InputSequenceId{0}, {}, 0);
    authority.recipient.owned_entity_ids = {
        kControlledEntity,
        dots::protocol::EntityId{10},
    };
    authority.owners = {{
        .owner_id = kControlledOwner,
        .last_input_id = dots::protocol::InputSequenceId{0},
        .split_cooldown_end_tick = 15,
    }};
    authority.entities = {
        {
            .entity_id = kControlledEntity,
            .kind = dots::protocol::EntityKind::Player,
            .owner_id = kControlledOwner,
            .mass = 8.0F,
            .merge_eligible_tick = 151,
        },
        {
            .entity_id = dots::protocol::EntityId{10},
            .kind = dots::protocol::EntityKind::Player,
            .owner_id = kControlledOwner,
            .mass = 8.0F,
            .merge_eligible_tick = 151,
            .prediction_key =
                dots::protocol::PredictionKey{
                    .owner_id = kControlledOwner,
                    .input_id = dots::protocol::InputSequenceId{0},
                    .child_ordinal = 0,
                },
        },
    };
    push_snapshot(endpoint, connection, authority);
    REQUIRE_FALSE(client.process_events(clock_time(11s)).has_value());

    REQUIRE(client.predicted_world() != nullptr);
    CHECK_FALSE(client.predicted_world()->contains(dots::simulation::EntityId{9}));
    CHECK(client.predicted_world()->contains(dots::simulation::EntityId{10}));
    REQUIRE(client.latest_prediction_identity_remaps().size() == 1);
    CHECK(client.latest_prediction_identity_remaps().front() ==
          dots::client_runtime::PredictionIdentityRemap{
              .prediction_key =
                  {
                      .owner_id = kControlledOwner,
                      .input_id = dots::protocol::InputSequenceId{0},
                      .child_ordinal = 0,
                  },
              .previous_entity_id = dots::protocol::EntityId{9},
              .current_entity_id = dots::protocol::EntityId{10},
          });
}

TEST_CASE("Predicted local elimination preserves confirmed play and buffered input",
          "[dots][prediction][session][rollback]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{26};
    complete_handshake(endpoint, client, connection);

    auto dangerous = snapshot(1, 0, dots::protocol::InputSequenceId::invalid(), {});
    dangerous.entities.push_back({
        .entity_id = dots::protocol::EntityId{9},
        .kind = dots::protocol::EntityKind::Player,
        .owner_id = dots::protocol::PlayerOwnerId{4},
        .mass = 32.0F,
    });
    push_snapshot(endpoint, connection, dangerous);
    REQUIRE_FALSE(client.process_events(clock_time(10s)).has_value());

    REQUIRE(client.send_input(0, {}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE(client.predicted_world() != nullptr);
    CHECK_FALSE(
        client.predicted_world()->contains(dots::simulation::EntityId{kControlledEntity.value()}));
    CHECK(client.predicted_owned_entity_ids().empty());
    CHECK(std::ranges::find(client.predicted_scope_entity_ids(), kControlledEntity) !=
          client.predicted_scope_entity_ids().end());
    CHECK(client.session_mode() == dots::protocol::SessionMode::Playing);
    check_position(client.predicted_position(), 0.0F, 0.0F);

    REQUIRE(client.send_input(1, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    CHECK(client.prediction_statistics(clock_time(10s)).history_count == 2);

    auto corrected = snapshot(2, 1, dots::protocol::InputSequenceId::invalid(), {});
    corrected.entities.push_back({
        .entity_id = dots::protocol::EntityId{9},
        .kind = dots::protocol::EntityKind::Player,
        .owner_id = dots::protocol::PlayerOwnerId{4},
        .mass = 16.0F,
    });
    push_snapshot(endpoint, connection, corrected);
    REQUIRE_FALSE(client.process_events(clock_time(11s)).has_value());

    REQUIRE(client.predicted_world() != nullptr);
    CHECK(
        client.predicted_world()->contains(dots::simulation::EntityId{kControlledEntity.value()}));
    REQUIRE(client.predicted_owned_entity_ids().size() == 1);
    CHECK(client.session_mode() == dots::protocol::SessionMode::Playing);
    CHECK(client.prediction_statistics(clock_time(11s)).history_count == 2);
    check_position(client.predicted_position(), 0.2F, 0.0F);
}

TEST_CASE("Client prediction closure follows retained replay depth instead of ring capacity",
          "[dots][prediction][scope][remote]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{28};
    complete_handshake(endpoint, client, connection);

    auto authority = snapshot(1, 0, dots::protocol::InputSequenceId::invalid(), {});
    auto remote_authority = dots::protocol::EntityState{
        .entity_id = dots::protocol::EntityId{9},
        .kind = dots::protocol::EntityKind::Player,
        .owner_id = dots::protocol::PlayerOwnerId{4},
        .position_x = 20.0F,
        .mass = 16.0F,
    };
    authority.entities.push_back(remote_authority);
    push_snapshot(endpoint, connection, authority);
    REQUIRE_FALSE(client.process_events(clock_time(10s)).has_value());

    const auto remote = dots::protocol::EntityId{9};
    CHECK(std::ranges::find(client.predicted_scope_entity_ids(), remote) ==
          client.predicted_scope_entity_ids().end());
    REQUIRE(client.predicted_world() != nullptr);
    CHECK_FALSE(client.predicted_world()->contains(dots::simulation::EntityId{remote.value()}));

    for (std::uint32_t tick = 0; tick < 6; ++tick) {
        REQUIRE(client.send_input(tick, {}) == dots::client_runtime::InputSendResult::Sent);
        CHECK(std::ranges::find(client.predicted_scope_entity_ids(), remote) ==
              client.predicted_scope_entity_ids().end());
    }

    const auto reconciliation_count =
        client.prediction_statistics(clock_time(10s)).reconciliation_count;
    REQUIRE(client.send_input(6, {}) == dots::client_runtime::InputSendResult::Sent);
    CHECK(std::ranges::find(client.predicted_scope_entity_ids(), remote) !=
          client.predicted_scope_entity_ids().end());
    REQUIRE(client.predicted_world() != nullptr);
    CHECK(client.predicted_world()->contains(dots::simulation::EntityId{remote.value()}));
    CHECK(client.prediction_statistics(clock_time(10s)).reconciliation_count ==
          reconciliation_count);
    const auto expanded_scope = client.prediction_statistics(clock_time(10s));
    CHECK(expanded_scope.scope_replay_horizon_ticks == 7);
    CHECK(expanded_scope.scope_owner_count == 2);

    auto acknowledged = snapshot(2, 7, dots::protocol::InputSequenceId{6}, {});
    remote_authority.position_x = 21.4F;
    acknowledged.entities.push_back(remote_authority);
    push_snapshot(endpoint, connection, acknowledged);
    REQUIRE_FALSE(client.process_events(clock_time(11s)).has_value());

    CHECK(std::ranges::find(client.predicted_scope_entity_ids(), remote) !=
          client.predicted_scope_entity_ids().end());
    REQUIRE(client.predicted_world() != nullptr);
    CHECK(client.predicted_world()->contains(dots::simulation::EntityId{remote.value()}));
    const auto retained_scope = client.prediction_statistics(clock_time(11s));
    CHECK(retained_scope.scope_epoch == expanded_scope.scope_epoch);
    CHECK(retained_scope.scope_replay_horizon_ticks == expanded_scope.scope_replay_horizon_ticks);
    CHECK(retained_scope.scope_rebase_count == expanded_scope.scope_rebase_count);
}

TEST_CASE("Future remote assumptions use newest authority after replaying an older guess",
          "[dots][prediction][remote][reconciliation]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{29};
    complete_handshake(endpoint, client, connection);

    auto remote = dots::protocol::EntityState{
        .entity_id = dots::protocol::EntityId{9},
        .kind = dots::protocol::EntityKind::Player,
        .owner_id = dots::protocol::PlayerOwnerId{4},
        .position_x = 10.0F,
        .mass = 16.0F,
    };
    auto authority = snapshot(1, 0, dots::protocol::InputSequenceId::invalid(), {});
    authority.owners.push_back({
        .owner_id = remote.owner_id,
        .movement_x = 1.0F,
        .last_non_zero_movement_x = 1.0F,
    });
    authority.entities.push_back(remote);
    push_snapshot(endpoint, connection, authority);
    REQUIRE_FALSE(client.process_events(clock_time(10s)).has_value());

    REQUIRE(client.send_input(0, {}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE(client.send_input(1, {}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE(client.predicted_world() != nullptr);
    check_position(client.predicted_world()->position(dots::simulation::EntityId{9}), 10.4F, 0.0F);

    remote.position_x = 10.2F;
    auto turned = snapshot(2, 1, dots::protocol::InputSequenceId{0}, {});
    turned.owners.push_back({
        .owner_id = remote.owner_id,
        .movement_y = 1.0F,
        .last_non_zero_movement_y = 1.0F,
    });
    turned.entities.push_back(remote);
    push_snapshot(endpoint, connection, turned);
    REQUIRE_FALSE(client.process_events(clock_time(11s)).has_value());

    REQUIRE(client.predicted_world() != nullptr);
    check_position(client.predicted_world()->position(dots::simulation::EntityId{9}), 10.2F, 0.2F);
    const auto corrections = client.recent_prediction_corrections();
    REQUIRE(corrections.size() == 1);
    CHECK(corrections.front().entity_id == dots::protocol::EntityId{9});
    CHECK(corrections.front().pre_correction_position == mycore::math::Vector2{10.4F, 0.0F});
    CHECK(corrections.front().corrected_position == mycore::math::Vector2{10.2F, 0.2F});
    CHECK(corrections.front().source == dots::client_runtime::PredictionCorrectionSource::Remote);
    const auto correction_statistics = client.prediction_statistics(clock_time(11s));
    CHECK(correction_statistics.latest_remote_entity_correction_count == 1);
    CHECK(correction_statistics.remote_entity_correction_count == 1);
    CHECK(correction_statistics.latest_remote_correction_distance ==
          Catch::Approx(std::sqrt(0.08F)));
    CHECK(correction_statistics.maximum_remote_correction_distance ==
          correction_statistics.latest_remote_correction_distance);
    REQUIRE(client.send_input(2, {}) == dots::client_runtime::InputSendResult::Sent);
    check_position(client.predicted_world()->position(dots::simulation::EntityId{9}), 10.2F, 0.4F);
}

TEST_CASE("Remote forward progress across different predicted head ticks is not a correction",
          "[dots][prediction][remote][reconciliation]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{30};
    complete_handshake(endpoint, client, connection);

    auto remote = dots::protocol::EntityState{
        .entity_id = dots::protocol::EntityId{9},
        .kind = dots::protocol::EntityKind::Player,
        .owner_id = dots::protocol::PlayerOwnerId{4},
        .position_x = 10.0F,
        .mass = 16.0F,
    };
    auto authority = snapshot(1, 0, dots::protocol::InputSequenceId::invalid(), {});
    authority.owners.push_back({
        .owner_id = remote.owner_id,
        .movement_x = 1.0F,
        .last_non_zero_movement_x = 1.0F,
    });
    authority.entities.push_back(remote);
    push_snapshot(endpoint, connection, authority);
    REQUIRE_FALSE(client.process_events(clock_time(10s)).has_value());
    REQUIRE(client.send_input(0, {}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE(client.predicted_world() != nullptr);
    check_position(client.predicted_world()->position(dots::simulation::EntityId{9}), 10.2F, 0.0F);

    remote.position_x = 10.4F;
    auto advanced = snapshot(2, 2, dots::protocol::InputSequenceId{0}, {});
    advanced.owners.push_back({
        .owner_id = remote.owner_id,
        .movement_x = 1.0F,
        .last_non_zero_movement_x = 1.0F,
    });
    advanced.entities.push_back(remote);
    push_snapshot(endpoint, connection, advanced);
    REQUIRE_FALSE(client.process_events(clock_time(11s)).has_value());

    REQUIRE(client.predicted_world() != nullptr);
    check_position(client.predicted_world()->position(dots::simulation::EntityId{9}), 10.4F, 0.0F);
    CHECK(client.recent_prediction_corrections().empty());
    const auto statistics = client.prediction_statistics(clock_time(11s));
    CHECK(statistics.latest_remote_entity_correction_count == 0);
    CHECK(statistics.remote_entity_correction_count == 0);
}

TEST_CASE("Misprediction corrects simulation immediately and records its replay path",
          "[dots][prediction][reconciliation]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{13};
    complete_handshake(endpoint, client, connection);
    REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);

    push_snapshot(endpoint,
                  connection,
                  snapshot(1, 1, dots::protocol::InputSequenceId::invalid(), {1.0F, 0.0F}));
    REQUIRE_FALSE(client.process_events(clock_time(11s)).has_value());

    check_position(client.predicted_position(), 1.2F, 0.0F);
    check_position(client.pre_correction_position(), 0.2F, 0.0F);
    REQUIRE(client.latest_replay_path().size() == 1);
    CHECK(client.latest_replay_path().front().x == Catch::Approx(1.2F));
    const auto statistics = client.prediction_statistics(clock_time(11s));
    CHECK(statistics.reconciliation_count == 1);
    CHECK(statistics.nonzero_correction_count == 1);
    CHECK(statistics.latest_correction_distance == Catch::Approx(1.0F));
    CHECK(statistics.maximum_correction_distance == Catch::Approx(1.0F));
    CHECK(statistics.corrections_per_minute == Catch::Approx(1.0F));
}

TEST_CASE("Stale snapshots cannot roll prediction or metrics backward",
          "[dots][prediction][reconciliation]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{14};
    complete_handshake(endpoint, client, connection);
    REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    push_snapshot(
        endpoint, connection, snapshot(1, 1, dots::protocol::InputSequenceId{0}, {0.2F, 0.0F}));
    REQUIRE_FALSE(client.process_events(clock_time(11s)).has_value());
    const auto before = client.prediction_statistics(clock_time(11s));
    const auto accepted_before =
        client.replication_statistics(clock_time(11s)).accepted_snapshot_count;

    push_snapshot(
        endpoint, connection, snapshot(1, 99, dots::protocol::InputSequenceId{99}, {99.0F, 99.0F}));
    REQUIRE_FALSE(client.process_events(clock_time(12s)).has_value());

    CHECK(client.world().snapshot_id() == dots::protocol::SnapshotId{1});
    check_position(client.predicted_position(), 0.2F, 0.0F);
    const auto after = client.prediction_statistics(clock_time(12s));
    CHECK(after.reconciliation_count == before.reconciliation_count);
    CHECK(after.history_count == before.history_count);
    CHECK(client.replication_statistics(clock_time(12s)).accepted_snapshot_count ==
          accepted_before);
}

TEST_CASE("Acknowledgement beyond the last sent input is rejected atomically",
          "[dots][prediction][ack]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{15};
    complete_handshake(endpoint, client, connection);
    REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);

    push_snapshot(
        endpoint, connection, snapshot(1, 1, dots::protocol::InputSequenceId{1}, {9.0F, 0.0F}));
    const auto result = client.process_events(clock_time(11s));
    REQUIRE(result.error == dots::client_runtime::RuntimeError::InvalidInputAcknowledgement);
    REQUIRE(client.state() == dots::client_runtime::State::Failed);
    CHECK(client.world().snapshot_id() == dots::protocol::SnapshotId{0});
    check_position(client.predicted_position(), 0.2F, 0.0F);
    const auto statistics = client.prediction_statistics(clock_time(11s));
    CHECK(statistics.history_count == 1);
    CHECK(statistics.reconciliation_count == 0);
    CHECK(endpoint.disconnect_called);
}

TEST_CASE("Acknowledgement cannot regress or become invalid", "[dots][prediction][ack]") {
    SECTION("regression") {
        ManualEndpoint endpoint;
        dots::client_runtime::Runtime client{endpoint};
        const mycore::net_transport::ConnectionHandle connection{16};
        complete_handshake(endpoint, client, connection);
        REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
        REQUIRE(client.send_input(1, {0.0F, 1.0F}) == dots::client_runtime::InputSendResult::Sent);
        push_snapshot(
            endpoint, connection, snapshot(1, 2, dots::protocol::InputSequenceId{1}, {0.2F, 0.2F}));
        REQUIRE_FALSE(client.process_events(clock_time(11s)).has_value());
        const auto before = client.prediction_statistics(clock_time(11s));

        push_snapshot(
            endpoint, connection, snapshot(2, 3, dots::protocol::InputSequenceId{0}, {9.0F, 0.0F}));
        REQUIRE(client.process_events(clock_time(12s)).error ==
                dots::client_runtime::RuntimeError::InvalidInputAcknowledgement);
        CHECK(client.world().snapshot_id() == dots::protocol::SnapshotId{1});
        check_position(client.predicted_position(), 0.2F, 0.2F);
        CHECK(client.prediction_statistics(clock_time(12s)).reconciliation_count ==
              before.reconciliation_count);
    }

    SECTION("valid acknowledgement becomes invalid") {
        ManualEndpoint endpoint;
        dots::client_runtime::Runtime client{endpoint};
        const mycore::net_transport::ConnectionHandle connection{17};
        complete_handshake(endpoint, client, connection);
        REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
        push_snapshot(
            endpoint, connection, snapshot(1, 1, dots::protocol::InputSequenceId{0}, {0.2F, 0.0F}));
        REQUIRE_FALSE(client.process_events(clock_time(11s)).has_value());

        push_snapshot(endpoint,
                      connection,
                      snapshot(2, 2, dots::protocol::InputSequenceId::invalid(), {9.0F, 0.0F}));
        REQUIRE(client.process_events(clock_time(12s)).error ==
                dots::client_runtime::RuntimeError::InvalidInputAcknowledgement);
        CHECK(client.world().snapshot_id() == dots::protocol::SnapshotId{1});
        check_position(client.predicted_position(), 0.2F, 0.0F);
    }
}

TEST_CASE("Confirmed spectating accepts a missing primary and clears prediction",
          "[dots][prediction][session]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{18};
    complete_handshake(endpoint, client, connection);
    REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    auto missing = snapshot(1, 1, dots::protocol::InputSequenceId::invalid(), {9.0F, 0.0F});
    missing.entities.clear();
    missing.recipient = {
        .mode = dots::protocol::SessionMode::Spectating,
        .defeat_tick = 1,
        .respawn_available_tick = 91,
    };
    push_snapshot(endpoint, connection, missing);

    REQUIRE_FALSE(client.process_events(clock_time(11s)).error.has_value());
    CHECK(client.world().snapshot_id() == dots::protocol::SnapshotId{1});
    CHECK(client.session_mode() == dots::protocol::SessionMode::Spectating);
    CHECK_FALSE(client.primary_entity_id().is_valid());
    CHECK_FALSE(client.predicted_position().has_value());
    CHECK(client.prediction_statistics(clock_time(11s)).history_count == 0);
    CHECK(client.send_input(1, {}, dots::protocol::kRespawnActionBit) ==
          dots::client_runtime::InputSendResult::Sent);
}

TEST_CASE("Client rejects a respawn deadline that conflicts with welcome configuration",
          "[dots][prediction][session]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{23};
    complete_handshake(endpoint, client, connection);
    auto invalid = snapshot(1, 1, dots::protocol::InputSequenceId::invalid(), {});
    invalid.entities.clear();
    invalid.recipient = {
        .mode = dots::protocol::SessionMode::Spectating,
        .defeat_tick = 1,
        .respawn_available_tick = 2,
    };
    push_snapshot(endpoint, connection, invalid);

    CHECK(client.process_events(clock_time(11s)).error ==
          dots::client_runtime::RuntimeError::InvalidSnapshot);
    CHECK(client.world().snapshot_id() == dots::protocol::SnapshotId{0});
}

TEST_CASE("Client rejects a corrupt checkpoint without replacing committed prediction",
          "[dots][prediction][transaction]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{27};
    complete_handshake(endpoint, client, connection);
    REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);

    dots::protocol::Message message =
        snapshot(1, 1, dots::protocol::InputSequenceId::invalid(), {1.0F, 0.0F});
    complete_protocol_fixture(message);
    auto& corrupt = std::get<dots::protocol::FullSnapshot>(message);
    corrupt.checkpoint_digest ^= 1U;
    endpoint.events.push_back(mycore::net_transport::PayloadReceived{
        .connection = connection,
        .delivery = mycore::net_transport::DeliveryMode::Unreliable,
        .payload = encode_finalized(message),
    });

    CHECK(client.process_events(clock_time(11s)).error ==
          dots::client_runtime::RuntimeError::CheckpointHydrationFailed);
    CHECK(client.world().snapshot_id() == dots::protocol::SnapshotId{0});
    CHECK(client.prediction_statistics(clock_time(11s)).history_count == 1);
    check_position(client.predicted_position(), 0.2F, 0.0F);
}

TEST_CASE("Prediction history capacity hard-resyncs before recording new input",
          "[dots][prediction][capacity]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    complete_handshake(endpoint, client, mycore::net_transport::ConnectionHandle{19});

    for (std::uint32_t tick = 0; tick < dots::client_runtime::kPredictionHistoryCapacity; ++tick) {
        REQUIRE(client.send_input(tick, {1.0F, 0.0F}) ==
                dots::client_runtime::InputSendResult::Sent);
    }
    auto statistics = client.prediction_statistics(clock_time(11s));
    CHECK(statistics.history_count == dots::client_runtime::kPredictionHistoryCapacity);
    CHECK(statistics.history_high_water_mark == dots::client_runtime::kPredictionHistoryCapacity);
    CHECK(statistics.hard_resync_count == 0);
    check_position(client.predicted_position(), 51.2F, 0.0F);

    REQUIRE(client.send_input(dots::client_runtime::kPredictionHistoryCapacity, {1.0F, 0.0F}) ==
            dots::client_runtime::InputSendResult::Sent);
    statistics = client.prediction_statistics(clock_time(11s));
    CHECK(statistics.history_count == 1);
    CHECK(statistics.history_high_water_mark == dots::client_runtime::kPredictionHistoryCapacity);
    CHECK(statistics.hard_resync_count == 1);
    CHECK(statistics.last_input_sent == dots::protocol::InputSequenceId{256});
    check_position(client.predicted_position(), 0.2F, 0.0F);
    CHECK_FALSE(client.pre_correction_position().has_value());
    CHECK(client.latest_replay_path().empty());

    REQUIRE_FALSE(endpoint.sent_payloads.empty());
    auto message = decode_bytes(endpoint.sent_payloads.back());
    const auto* packet = std::get_if<dots::protocol::InputPacket>(&message);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->samples.size() == 1);
    CHECK(packet->samples.front().sequence_id == dots::protocol::InputSequenceId{256});
}

TEST_CASE("Injected input drops suppress transport while preserving prediction",
          "[dots][prediction][debug]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    complete_handshake(endpoint, client, mycore::net_transport::ConnectionHandle{20});

    CHECK_FALSE(client.debug_drop_next_input_packets(0));
    REQUIRE(client.debug_drop_next_input_packets(3));
    CHECK(client.prediction_statistics(clock_time(10s)).pending_injected_input_drop_count == 3);

    for (std::uint32_t tick = 0; tick < 3; ++tick) {
        REQUIRE(client.send_input(tick, {1.0F, 0.0F}) ==
                dots::client_runtime::InputSendResult::Sent);
    }
    CHECK(endpoint.sent_payloads.empty());
    check_position(client.predicted_position(), 0.6F, 0.0F);
    auto statistics = client.prediction_statistics(clock_time(11s));
    CHECK(statistics.history_count == 3);
    CHECK(statistics.pending_injected_input_drop_count == 0);
    CHECK(statistics.injected_input_drop_count == 3);

    REQUIRE(client.send_input(3, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    REQUIRE(endpoint.sent_payloads.size() == 1);
    auto message = decode_bytes(endpoint.sent_payloads.front());
    const auto* packet = std::get_if<dots::protocol::InputPacket>(&message);
    REQUIRE(packet != nullptr);
    REQUIRE(packet->samples.size() == 3);
    CHECK(packet->samples[0].sequence_id == dots::protocol::InputSequenceId{1});
    CHECK(packet->samples[1].sequence_id == dots::protocol::InputSequenceId{2});
    CHECK(packet->samples[2].sequence_id == dots::protocol::InputSequenceId{3});
}

TEST_CASE("Injected prediction error is corrected and exposed separately from packet loss",
          "[dots][prediction][debug]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{21};
    complete_handshake(endpoint, client, connection);
    REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);

    CHECK_FALSE(client.debug_inject_prediction_error({}));
    REQUIRE(client.debug_inject_prediction_error({1.0F, 0.0F}));
    REQUIRE(client.debug_inject_prediction_error({0.0F, 1.0F}));
    check_position(client.predicted_position(), 1.2F, 1.0F);
    auto statistics = client.prediction_statistics(clock_time(10s));
    CHECK(statistics.injected_prediction_error_count == 2);
    CHECK(statistics.injected_input_drop_count == 0);

    push_snapshot(
        endpoint, connection, snapshot(1, 1, dots::protocol::InputSequenceId{0}, {0.2F, 0.0F}));
    REQUIRE_FALSE(client.process_events(clock_time(11s)).has_value());
    check_position(client.predicted_position(), 0.2F, 0.0F);
    check_position(client.pre_correction_position(), 1.2F, 1.0F);
    statistics = client.prediction_statistics(clock_time(11s));
    CHECK(statistics.latest_correction_distance == Catch::Approx(std::sqrt(2.0F)));
    CHECK(statistics.accumulated_correction_displacement.x == Catch::Approx(1.0F));
    CHECK(statistics.accumulated_correction_displacement.y == Catch::Approx(1.0F));
    CHECK(client.latest_correction_replay_path().empty());
    REQUIRE(client.recent_prediction_corrections().size() == 1);
    CHECK(client.recent_prediction_corrections().front().entity_id == kControlledEntity);
    CHECK(client.recent_prediction_corrections().front().source ==
          dots::client_runtime::PredictionCorrectionSource::Local);
    CHECK(client.recent_prediction_corrections().front().pre_correction_position ==
          mycore::math::Vector2{1.2F, 1.0F});
}

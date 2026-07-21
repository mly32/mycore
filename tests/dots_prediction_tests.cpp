#include "dots/client_runtime/client_runtime.hpp"
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
        .entities = {{
            .entity_id = kControlledEntity,
            .kind = dots::protocol::EntityKind::Player,
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
            .controlled_entity_id = kControlledEntity,
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

    REQUIRE(client.send_input(1, {std::numeric_limits<float>::infinity(), 0.0F}) ==
            dots::client_runtime::InputSendResult::InvalidMovement);
    check_position(client.predicted_position(), 0.2F, 0.0F);
    CHECK(client.prediction_statistics(clock_time(10s)).history_count == 1);
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
    const auto error = client.process_events(clock_time(11s));
    REQUIRE(error == dots::client_runtime::RuntimeError::InvalidInputAcknowledgement);
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
        REQUIRE(client.process_events(clock_time(12s)) ==
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
        REQUIRE(client.process_events(clock_time(12s)) ==
                dots::client_runtime::RuntimeError::InvalidInputAcknowledgement);
        CHECK(client.world().snapshot_id() == dots::protocol::SnapshotId{1});
        check_position(client.predicted_position(), 0.2F, 0.0F);
    }
}

TEST_CASE("Missing controlled entity is rejected before snapshot commit",
          "[dots][prediction][reconciliation]") {
    ManualEndpoint endpoint;
    dots::client_runtime::Runtime client{endpoint};
    const mycore::net_transport::ConnectionHandle connection{18};
    complete_handshake(endpoint, client, connection);
    REQUIRE(client.send_input(0, {1.0F, 0.0F}) == dots::client_runtime::InputSendResult::Sent);
    auto missing = snapshot(1, 1, dots::protocol::InputSequenceId::invalid(), {9.0F, 0.0F});
    missing.entities.clear();
    push_snapshot(endpoint, connection, missing);

    REQUIRE(client.process_events(clock_time(11s)) ==
            dots::client_runtime::RuntimeError::MissingControlledEntity);
    CHECK(client.world().snapshot_id() == dots::protocol::SnapshotId{0});
    check_position(client.predicted_position(), 0.2F, 0.0F);
    CHECK(client.prediction_statistics(clock_time(11s)).history_count == 1);
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
}

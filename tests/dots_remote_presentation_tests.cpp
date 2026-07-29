#include "dots/presentation/remote_presentation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] constexpr std::chrono::steady_clock::time_point
clock_time(std::chrono::steady_clock::duration offset) noexcept {
    return std::chrono::steady_clock::time_point{offset};
}

[[nodiscard]] dots::presentation::RemoteSnapshotSample snapshot(std::uint32_t snapshot_id,
                                                                std::uint32_t server_tick,
                                                                float remote_x,
                                                                std::chrono::milliseconds arrival,
                                                                bool include_remote = true) {
    std::vector<dots::protocol::EntityState> entities{
        {
            .entity_id = dots::protocol::EntityId{1},
            .kind = dots::protocol::EntityKind::Player,
            .position_x = 50.0F,
            .position_y = 0.0F,
            .mass = 16.0F,
        },
    };
    if (include_remote) {
        entities.push_back({
            .entity_id = dots::protocol::EntityId{2},
            .kind = dots::protocol::EntityKind::Player,
            .position_x = remote_x,
            .position_y = 3.0F,
            .mass = 4.0F,
        });
    }
    return {
        .snapshot_id = dots::protocol::SnapshotId{snapshot_id},
        .server_tick = server_tick,
        .entities = std::move(entities),
        .arrival_time = clock_time(arrival),
    };
}

[[nodiscard]] dots::presentation::RemoteKinematicSnapshot kinematic_snapshot(
    std::uint32_t snapshot_id, std::uint32_t server_tick, std::chrono::milliseconds arrival) {
    auto rules = dots::simulation::WorldRules{};
    rules.player_speed_units_per_second = 30.0F;
    rules.launch_decay_units_per_second_squared = 0.0F;
    return {
        .snapshot_id = dots::protocol::SnapshotId{snapshot_id},
        .server_tick = server_tick,
        .rules = rules,
        .owners = {{
            .owner_id = dots::protocol::PlayerOwnerId{5},
            .movement_x = 1.0F,
            .movement_y = 0.0F,
        }},
        .entities =
            {
                {
                    .entity_id = dots::protocol::EntityId{2},
                    .kind = dots::protocol::EntityKind::Player,
                    .owner_id = dots::protocol::PlayerOwnerId{5},
                    .position_x = 0.0F,
                    .position_y = 3.0F,
                    .mass = 4.0F,
                    .launch_velocity_x = 30.0F,
                    .prediction_key =
                        dots::protocol::PredictionKey{
                            .owner_id = dots::protocol::PlayerOwnerId{5},
                            .input_id = dots::protocol::InputSequenceId{7},
                            .child_ordinal = 1,
                        },
                },
                {
                    .entity_id = dots::protocol::EntityId{3},
                    .kind = dots::protocol::EntityKind::Food,
                    .position_x = 9.0F,
                    .position_y = 8.0F,
                    .mass = 1.0F,
                },
            },
        .arrival_time = clock_time(arrival),
    };
}

} // namespace

TEST_CASE("Remote extrapolation advances movement and launch but keeps food static",
          "[dots][remote-presentation][extrapolation]") {
    dots::presentation::RemoteExtrapolationBuffer buffer;
    REQUIRE(buffer.insert(kinematic_snapshot(1, 40, 0ms)));

    const auto frame = buffer.sample(clock_time(116666667ns));
    REQUIRE(frame.ready);
    REQUIRE_FALSE(frame.holding);
    CHECK(frame.extrapolation_ticks == Catch::Approx(3.5));
    REQUIRE(frame.entities.size() == 2);
    CHECK(frame.entities[0].position.x == Catch::Approx(7.0F));
    CHECK(frame.entities[0].position.y == Catch::Approx(3.0F));
    CHECK(frame.entities[0].owner_id == dots::protocol::PlayerOwnerId{5});
    REQUIRE(frame.entities[0].prediction_key.has_value());
    CHECK(frame.entities[0].prediction_key->input_id == dots::protocol::InputSequenceId{7});
    CHECK(frame.entities[1].position.x == Catch::Approx(9.0F));
    CHECK(frame.entities[1].position.y == Catch::Approx(8.0F));

    const auto statistics = buffer.statistics(clock_time(116666667ns));
    CHECK(statistics.extrapolated_player_count == 1);
    CHECK(statistics.static_entity_count == 1);
    CHECK(statistics.held_player_count == 0);
}

TEST_CASE("Remote extrapolation holds after its six-tick horizon",
          "[dots][remote-presentation][extrapolation]") {
    dots::presentation::RemoteExtrapolationBuffer buffer;
    REQUIRE(buffer.insert(kinematic_snapshot(1, 40, 0ms)));

    const auto frame = buffer.sample(clock_time(500ms));
    REQUIRE(frame.ready);
    REQUIRE(frame.holding);
    CHECK(frame.extrapolation_ticks ==
          Catch::Approx(dots::presentation::kRemoteExtrapolationLimitTicks));
    REQUIRE(frame.entities.size() == 2);
    CHECK(frame.entities[0].position.x == Catch::Approx(12.0F));

    const auto statistics = buffer.statistics(clock_time(500ms));
    CHECK(statistics.extrapolated_player_count == 0);
    CHECK(statistics.held_player_count == 1);
}

TEST_CASE("Remote extrapolation rejects invalid and stale authoritative samples",
          "[dots][remote-presentation][extrapolation]") {
    dots::presentation::RemoteExtrapolationBuffer buffer;
    REQUIRE(buffer.insert(kinematic_snapshot(2, 40, 0ms)));
    CHECK_FALSE(buffer.insert(kinematic_snapshot(1, 42, 1ms)));
    CHECK_FALSE(buffer.insert(kinematic_snapshot(3, 39, 1ms)));
    REQUIRE(buffer.insert(kinematic_snapshot(3, 40, 1ms)));

    auto invalid_velocity = kinematic_snapshot(4, 42, 1ms);
    invalid_velocity.entities[0].launch_velocity_x = std::numeric_limits<float>::quiet_NaN();
    const auto accepted_invalid_velocity = buffer.insert(std::move(invalid_velocity));
    CHECK_FALSE(accepted_invalid_velocity);
    auto invalid_movement = kinematic_snapshot(4, 42, 1ms);
    invalid_movement.owners[0].movement_x = 2.0F;
    const auto accepted_invalid_movement = buffer.insert(std::move(invalid_movement));
    CHECK_FALSE(accepted_invalid_movement);
    auto unsorted_owners = kinematic_snapshot(4, 42, 1ms);
    unsorted_owners.owners.push_back({
        .owner_id = dots::protocol::PlayerOwnerId{4},
        .movement_x = 0.0F,
        .movement_y = 1.0F,
    });
    const auto accepted_unsorted_owners = buffer.insert(std::move(unsorted_owners));
    CHECK_FALSE(accepted_unsorted_owners);

    const auto statistics = buffer.statistics(clock_time(1ms));
    CHECK(statistics.accepted_snapshot_count == 2);
    CHECK(statistics.rejected_snapshot_count == 5);
    CHECK(statistics.snapshot_id == dots::protocol::SnapshotId{3});
}

TEST_CASE("Remote presentation holds the first sample while its normal delay buffers",
          "[dots][remote-presentation]") {
    dots::presentation::RemoteSnapshotBuffer buffer;
    buffer.insert(snapshot(1, 40, 4.0F, 0ms));
    buffer.advance(clock_time(0ms));

    const auto first = buffer.sample(dots::protocol::EntityId{1});
    REQUIRE_FALSE(first.ready);
    REQUIRE(first.entities.size() == 1);
    CHECK(first.presentation_tick == Catch::Approx(40.0));
    CHECK(first.entities[0].position.x == Catch::Approx(4.0F));
    CHECK(buffer.statistics(clock_time(0ms)).current_delay_ticks == Catch::Approx(0.0));

    buffer.insert(snapshot(2, 42, 6.0F, 67ms));
    buffer.insert(snapshot(3, 44, 8.0F, 133ms));
    buffer.advance(clock_time(133ms));
    const auto buffering = buffer.sample(dots::protocol::EntityId{1});
    REQUIRE_FALSE(buffering.ready);
    CHECK(buffering.presentation_tick == Catch::Approx(40.0));
    CHECK(buffering.entities[0].position.x == Catch::Approx(4.0F));

    buffer.insert(snapshot(4, 46, 10.0F, 200ms));
    buffer.advance(clock_time(200ms));
    const auto ready = buffer.sample(dots::protocol::EntityId{1});
    REQUIRE(ready.ready);
    CHECK(ready.presentation_tick == Catch::Approx(40.0));
}

TEST_CASE("Remote presentation waits for six ticks then samples known endpoints",
          "[dots][remote-presentation]") {
    dots::presentation::RemoteSnapshotBuffer buffer;
    buffer.insert(snapshot(1, 0, 0.0F, 0ms));
    buffer.insert(snapshot(2, 2, 2.0F, 67ms));
    buffer.insert(snapshot(3, 4, 4.0F, 133ms));
    buffer.insert(snapshot(4, 6, 6.0F, 200ms));
    buffer.advance(clock_time(200ms));

    const auto frame = buffer.sample(dots::protocol::EntityId{1});
    REQUIRE(frame.ready);
    REQUIRE(frame.bracket.has_value());
    CHECK(frame.presentation_tick == Catch::Approx(0.0));
    REQUIRE(frame.entities.size() == 1);
    CHECK(frame.entities[0].entity_id == dots::protocol::EntityId{2});
    CHECK(frame.entities[0].position.x == Catch::Approx(0.0F));

    buffer.advance(clock_time(200ms + 33ms));
    const auto interpolated = buffer.sample(dots::protocol::EntityId{1});
    REQUIRE(interpolated.bracket.has_value());
    CHECK(interpolated.bracket->alpha == Catch::Approx(0.5F).margin(0.02F));
    CHECK(interpolated.entities[0].position.x == Catch::Approx(1.0F).margin(0.04F));
}

TEST_CASE("Remote presentation holds on an underrun and does not extrapolate",
          "[dots][remote-presentation]") {
    dots::presentation::RemoteSnapshotBuffer buffer;
    buffer.insert(snapshot(1, 0, 0.0F, 0ms));
    buffer.insert(snapshot(2, 2, 2.0F, 67ms));
    buffer.insert(snapshot(3, 4, 4.0F, 133ms));
    buffer.insert(snapshot(4, 6, 6.0F, 200ms));
    buffer.advance(clock_time(200ms));
    buffer.advance(clock_time(500ms));

    const auto held = buffer.sample(dots::protocol::EntityId{1});
    REQUIRE(held.holding);
    REQUIRE(held.entities.size() == 1);
    CHECK(held.entities[0].position.x == Catch::Approx(6.0F));
    CHECK(held.presentation_tick == Catch::Approx(9.0));

    buffer.advance(clock_time(516ms));
    const auto still_held = buffer.sample(dots::protocol::EntityId{1});
    REQUIRE(still_held.holding);
    CHECK(still_held.presentation_tick == Catch::Approx(held.presentation_tick));
    CHECK(still_held.entities[0].position.x == Catch::Approx(6.0F));
    const auto statistics = buffer.statistics(clock_time(550ms));
    CHECK(statistics.hold_episode_count == 1);
    CHECK(statistics.current_hold_duration >= 50ms);
    CHECK(statistics.last_hold_duration == 0ms);
    CHECK(statistics.maximum_hold_duration >= 50ms);
    CHECK(statistics.total_hold_duration >= 50ms);
    CHECK(statistics.hold_recovery_count == 0);
    CHECK(statistics.cursor_rate == Catch::Approx(0.0));
    CHECK(statistics.hard_rebase_count == 0);
    CHECK(statistics.holding);
}

TEST_CASE("Remote presentation recovers monotonically through late snapshots",
          "[dots][remote-presentation]") {
    dots::presentation::RemoteSnapshotBuffer buffer;
    buffer.insert(snapshot(1, 0, 0.0F, 0ms));
    buffer.insert(snapshot(2, 2, 2.0F, 67ms));
    buffer.insert(snapshot(3, 4, 4.0F, 133ms));
    buffer.insert(snapshot(4, 6, 6.0F, 200ms));
    buffer.advance(clock_time(200ms));
    buffer.advance(clock_time(500ms));
    buffer.advance(clock_time(516ms));
    const auto held_tick = buffer.sample(dots::protocol::EntityId{1}).presentation_tick;

    buffer.insert(snapshot(5, 8, 8.0F, 510ms));
    buffer.advance(clock_time(550ms));
    const auto late = buffer.sample(dots::protocol::EntityId{1});
    REQUIRE(late.holding);
    CHECK(late.presentation_tick == Catch::Approx(held_tick));
    REQUIRE(late.entities.size() == 1);
    CHECK(late.entities[0].position.x == Catch::Approx(8.0F));

    buffer.insert(snapshot(6, 10, 10.0F, 567ms));
    buffer.insert(snapshot(7, 12, 12.0F, 568ms));
    buffer.insert(snapshot(8, 14, 14.0F, 569ms));
    buffer.advance(clock_time(600ms));
    const auto recovered = buffer.sample(dots::protocol::EntityId{1});
    REQUIRE_FALSE(recovered.holding);
    CHECK(recovered.presentation_tick > held_tick);
    REQUIRE(recovered.entities.size() == 1);
    CHECK(recovered.entities[0].position.x >= 10.0F);

    const auto statistics = buffer.statistics(clock_time(600ms));
    CHECK(statistics.late_snapshot_count == 1);
    CHECK(statistics.hold_episode_count == 1);
    CHECK(statistics.hold_recovery_count == 1);
    CHECK(statistics.last_hold_duration == 100ms);
    CHECK(statistics.maximum_hold_duration == 100ms);
    CHECK(statistics.total_hold_duration == 100ms);
    CHECK_FALSE(statistics.holding);
    CHECK(statistics.hard_rebase_count == 0);
}

TEST_CASE("Remote presentation delays entity removal until the newer endpoint",
          "[dots][remote-presentation]") {
    dots::presentation::RemoteSnapshotBuffer buffer;
    buffer.insert(snapshot(1, 0, 0.0F, 0ms));
    buffer.insert(snapshot(2, 2, 2.0F, 67ms));
    buffer.insert(snapshot(3, 4, 4.0F, 133ms));
    buffer.insert(snapshot(4, 6, 6.0F, 200ms));
    buffer.insert(snapshot(5, 8, 8.0F, 267ms, false));
    buffer.advance(clock_time(267ms));
    const auto before_removal = buffer.sample(dots::protocol::EntityId{1});
    REQUIRE(before_removal.entities.size() == 1);
    buffer.advance(clock_time(267ms + 200ms));

    const auto frame = buffer.sample(dots::protocol::EntityId{1});
    CHECK(frame.entities.empty());
    CHECK(buffer.statistics(clock_time(467ms)).delayed_entity_remove_count == 1);
}

TEST_CASE("Remote presentation recreates an entity only after an absence endpoint",
          "[dots][remote-presentation]") {
    dots::presentation::RemoteSnapshotBuffer buffer;
    buffer.insert(snapshot(1, 0, 0.0F, 0ms));
    buffer.insert(snapshot(2, 2, 0.0F, 67ms, false));
    buffer.insert(snapshot(3, 4, 40.0F, 133ms));
    buffer.insert(snapshot(4, 6, 60.0F, 200ms));
    buffer.advance(clock_time(200ms));
    REQUIRE(buffer.sample(dots::protocol::EntityId{1}).entities.size() == 1);

    buffer.advance(clock_time(267ms));
    const auto absent = buffer.sample(dots::protocol::EntityId{1});
    CHECK(absent.entities.empty());
    auto statistics = buffer.statistics(clock_time(267ms));
    CHECK(statistics.delayed_entity_remove_count == 1);
    CHECK(statistics.delayed_entity_create_count == 0);

    buffer.advance(clock_time(337ms));
    const auto recreated = buffer.sample(dots::protocol::EntityId{1});
    REQUIRE(recreated.entities.size() == 1);
    CHECK(recreated.entities[0].position.x >= 40.0F);
    statistics = buffer.statistics(clock_time(337ms));
    CHECK(statistics.delayed_entity_remove_count == 1);
    CHECK(statistics.delayed_entity_create_count == 1);
}

TEST_CASE("Remote presentation interpolates across a lost snapshot gap",
          "[dots][remote-presentation]") {
    dots::presentation::RemoteSnapshotBuffer buffer;
    buffer.insert(snapshot(1, 0, 0.0F, 0ms));
    buffer.insert(snapshot(2, 6, 6.0F, 200ms));
    buffer.advance(clock_time(200ms));
    static_cast<void>(buffer.sample(dots::protocol::EntityId{1}));

    buffer.advance(clock_time(300ms));
    const auto frame = buffer.sample(dots::protocol::EntityId{1});
    REQUIRE(frame.bracket.has_value());
    CHECK(frame.presentation_tick == Catch::Approx(3.0));
    CHECK(frame.bracket->alpha == Catch::Approx(0.5F));
    REQUIRE(frame.entities.size() == 1);
    CHECK(frame.entities[0].position.x == Catch::Approx(3.0F));
}

TEST_CASE("Remote presentation bounds history and records arrival jitter",
          "[dots][remote-presentation]") {
    dots::presentation::RemoteSnapshotBuffer buffer;
    for (std::uint32_t index = 0; index < 33; ++index) {
        buffer.insert(snapshot(index + 1,
                               index * 2,
                               static_cast<float>(index * 2),
                               std::chrono::milliseconds{index * 67}));
    }

    const auto statistics = buffer.statistics(clock_time(2200ms));
    CHECK(statistics.sample_count == dots::presentation::kRemoteSnapshotCapacity);
    CHECK(statistics.coverage_ticks == 62);
    CHECK(statistics.latest_jitter_milliseconds == Catch::Approx(1.0 / 3.0).margin(0.01));
    CHECK(statistics.ewma_jitter_milliseconds > 0.0);
}

TEST_CASE("Remote presentation hard-rebases after a bounded cursor error",
          "[dots][remote-presentation]") {
    dots::presentation::RemoteSnapshotBuffer buffer;
    buffer.insert(snapshot(1, 0, 0.0F, 0ms));
    buffer.insert(snapshot(2, 2, 2.0F, 67ms));
    buffer.insert(snapshot(3, 4, 4.0F, 133ms));
    buffer.insert(snapshot(4, 6, 6.0F, 200ms));
    buffer.advance(clock_time(200ms));

    buffer.insert(snapshot(5, 20, 20.0F, 267ms));
    buffer.advance(clock_time(267ms));

    const auto statistics = buffer.statistics(clock_time(267ms));
    CHECK(statistics.hard_rebase_count == 1);
    CHECK(statistics.presentation_tick == Catch::Approx(14.0));
    CHECK(statistics.current_delay_ticks == Catch::Approx(6.0));
}

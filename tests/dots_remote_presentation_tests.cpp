#include "dots/presentation/remote_presentation.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
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

} // namespace

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
    const auto statistics = buffer.statistics(clock_time(550ms));
    CHECK(statistics.hold_episode_count == 1);
    CHECK(statistics.current_hold_duration >= 50ms);
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
    buffer.advance(clock_time(267ms + 200ms));

    const auto frame = buffer.sample(dots::protocol::EntityId{1});
    CHECK(frame.entities.empty());
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

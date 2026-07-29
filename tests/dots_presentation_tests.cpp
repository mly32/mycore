#include "dots/presentation/presentation.hpp"
#include "dots/presentation/spectator_camera.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] constexpr std::chrono::steady_clock::time_point
clock_time(std::chrono::steady_clock::duration offset) noexcept {
    return std::chrono::steady_clock::time_point{offset};
}

[[nodiscard]] dots::protocol::RecipientSessionState
playing_session(dots::protocol::EntityId primary_entity_id) {
    return {
        .mode = dots::protocol::SessionMode::Playing,
        .owned_entity_ids = {primary_entity_id},
        .primary_entity_id = primary_entity_id,
    };
}

[[nodiscard]] dots::presentation::FrameData
player_frame(float position_x,
             std::uint64_t revision,
             dots::presentation::PresentationSource source =
                 dots::presentation::PresentationSource::Predicted,
             dots::protocol::EntityId entity_id = dots::protocol::EntityId{1},
             std::optional<dots::protocol::PredictionKey> prediction_key = std::nullopt) {
    return {
        .camera = {},
        .circles = {{
            .position = {position_x, 0.0F},
            .mass = 16.0F,
            .radius = 4.0F,
            .kind = dots::presentation::CircleKind::Player,
            .entity_id = entity_id,
            .prediction_key = prediction_key,
            .source = source,
            .source_revision = revision,
        }},
    };
}

} // namespace

TEST_CASE("Persistent presentation interpolates predicted fixed-tick state",
          "[dots][presentation][persistent]") {
    dots::presentation::PersistentWorldPresentation presentation;
    auto first = presentation.compose(player_frame(0.0F, 1), 0.0F, 0, {}, clock_time(0ms));
    REQUIRE(first.circles.size() == 1);
    CHECK(first.circles.front().position.x == Catch::Approx(0.0F));

    auto advanced = presentation.compose(player_frame(8.0F, 2), 0.25F, 0, {}, clock_time(10ms));
    REQUIRE(advanced.circles.size() == 1);
    CHECK(advanced.circles.front().position.x == Catch::Approx(2.0F));

    auto later_render = presentation.compose(player_frame(8.0F, 2), 0.75F, 0, {}, clock_time(20ms));
    REQUIRE(later_render.circles.size() == 1);
    CHECK(later_render.circles.front().position.x == Catch::Approx(6.0F));
    CHECK(presentation.statistics().smoothed_correction_count == 0);
}

TEST_CASE("Persistent presentation passes through an already presented local state",
          "[dots][presentation][persistent]") {
    dots::presentation::PersistentWorldPresentation presentation;
    static_cast<void>(
        presentation.compose(player_frame(0.0F, 1, dots::presentation::PresentationSource::State),
                             0.0F,
                             0,
                             {},
                             clock_time(0ms)));

    const auto advanced =
        presentation.compose(player_frame(8.0F, 2, dots::presentation::PresentationSource::State),
                             0.25F,
                             0,
                             {},
                             clock_time(10ms));
    REQUIRE(advanced.circles.size() == 1);
    CHECK(advanced.circles.front().position.x == Catch::Approx(8.0F));
    CHECK(presentation.statistics().smoothed_correction_count == 0);
}

TEST_CASE("Persistent presentation does not smooth delayed interpolation bracket changes",
          "[dots][presentation][persistent]") {
    dots::presentation::PersistentWorldPresentation presentation;
    static_cast<void>(presentation.compose(
        player_frame(0.0F, 1, dots::presentation::PresentationSource::Interpolated),
        0.0F,
        0,
        {},
        clock_time(0ms)));

    const auto within_bracket = presentation.compose(
        player_frame(2.0F, 1, dots::presentation::PresentationSource::Interpolated),
        0.0F,
        0,
        {},
        clock_time(10ms));
    REQUIRE(within_bracket.circles.size() == 1);
    CHECK(within_bracket.circles.front().position.x == Catch::Approx(2.0F));

    const auto next_bracket = presentation.compose(
        player_frame(4.0F, 2, dots::presentation::PresentationSource::Interpolated),
        0.0F,
        0,
        {},
        clock_time(20ms));
    REQUIRE(next_bracket.circles.size() == 1);
    CHECK(next_bracket.circles.front().position.x == Catch::Approx(4.0F));
    CHECK(presentation.statistics().smoothed_correction_count == 0);
}

TEST_CASE("Persistent presentation smooths new extrapolation authority",
          "[dots][presentation][persistent]") {
    dots::presentation::PersistentWorldPresentation presentation;
    static_cast<void>(presentation.compose(
        player_frame(0.0F, 1, dots::presentation::PresentationSource::Extrapolated),
        0.0F,
        0,
        {},
        clock_time(0ms)));

    const auto advanced_guess = presentation.compose(
        player_frame(4.0F, 1, dots::presentation::PresentationSource::Extrapolated),
        0.0F,
        0,
        {},
        clock_time(10ms));
    REQUIRE(advanced_guess.circles.size() == 1);
    CHECK(advanced_guess.circles.front().position.x == Catch::Approx(4.0F));
    CHECK(presentation.statistics().smoothed_correction_count == 0);

    const auto corrected = presentation.compose(
        player_frame(2.0F, 2, dots::presentation::PresentationSource::Extrapolated),
        0.0F,
        0,
        {},
        clock_time(20ms));
    REQUIRE(corrected.circles.size() == 1);
    CHECK(corrected.circles.front().position.x == Catch::Approx(4.0F));
    CHECK(presentation.statistics().smoothed_correction_count == 1);

    const auto halfway = presentation.compose(
        player_frame(3.0F, 2, dots::presentation::PresentationSource::Extrapolated),
        0.0F,
        0,
        {},
        clock_time(70ms));
    REQUIRE(halfway.circles.size() == 1);
    CHECK(halfway.circles.front().position.x == Catch::Approx(4.0F));

    const auto settled = presentation.compose(
        player_frame(4.0F, 2, dots::presentation::PresentationSource::Extrapolated),
        0.0F,
        0,
        {},
        clock_time(120ms));
    REQUIRE(settled.circles.size() == 1);
    CHECK(settled.circles.front().position.x == Catch::Approx(4.0F));
}

TEST_CASE("Persistent presentation preserves PredictionKey identity across authority handoff",
          "[dots][presentation][persistent]") {
    const auto prediction_key = dots::protocol::PredictionKey{
        .owner_id = dots::protocol::PlayerOwnerId{4},
        .input_id = dots::protocol::InputSequenceId{7},
        .child_ordinal = 1,
    };
    dots::presentation::PersistentWorldPresentation presentation;
    static_cast<void>(
        presentation.compose(player_frame(0.0F,
                                          1,
                                          dots::presentation::PresentationSource::Predicted,
                                          dots::protocol::EntityId{100},
                                          prediction_key),
                             1.0F,
                             0,
                             {},
                             clock_time(0ms)));

    const auto handoff =
        presentation.compose(player_frame(10.0F,
                                          2,
                                          dots::presentation::PresentationSource::Extrapolated,
                                          dots::protocol::EntityId{200},
                                          prediction_key),
                             1.0F,
                             0,
                             {},
                             clock_time(10ms));
    REQUIRE(handoff.circles.size() == 1);
    CHECK(handoff.circles.front().entity_id == dots::protocol::EntityId{200});
    CHECK(handoff.circles.front().position.x == Catch::Approx(0.0F));

    const auto halfway =
        presentation.compose(player_frame(10.0F,
                                          2,
                                          dots::presentation::PresentationSource::Extrapolated,
                                          dots::protocol::EntityId{200},
                                          prediction_key),
                             1.0F,
                             0,
                             {},
                             clock_time(60ms));
    REQUIRE(halfway.circles.size() == 1);
    CHECK(halfway.circles.front().position.x == Catch::Approx(5.0F));
    CHECK(presentation.statistics().source_handoff_count == 1);
    CHECK(presentation.statistics().identity_remap_count == 1);
    CHECK(presentation.statistics().smoothed_correction_count == 1);
}

TEST_CASE("Persistent presentation smooths a same-head predicted correction",
          "[dots][presentation][persistent]") {
    dots::presentation::PersistentWorldPresentation presentation;
    static_cast<void>(presentation.compose(player_frame(0.0F, 3), 1.0F, 0, {}, clock_time(0ms)));

    const auto corrected =
        presentation.compose(player_frame(10.0F, 3), 1.0F, 0, {}, clock_time(10ms));
    REQUIRE(corrected.circles.size() == 1);
    CHECK(corrected.circles.front().position.x == Catch::Approx(0.0F));

    const auto halfway =
        presentation.compose(player_frame(10.0F, 3), 1.0F, 0, {}, clock_time(60ms));
    REQUIRE(halfway.circles.size() == 1);
    CHECK(halfway.circles.front().position.x == Catch::Approx(5.0F));
    CHECK(presentation.statistics().smoothed_correction_count == 1);
}

TEST_CASE("Persistent presentation bounds structural fades and motion trails",
          "[dots][presentation][persistent]") {
    dots::presentation::PersistentWorldPresentation presentation;
    for (std::uint64_t index = 0; index < 10; ++index) {
        static_cast<void>(presentation.compose(player_frame(static_cast<float>(index), index + 1),
                                               1.0F,
                                               0,
                                               dots::protocol::EntityId{1},
                                               clock_time(std::chrono::milliseconds{31 * index})));
    }
    CHECK(presentation.statistics().motion_trail_count == dots::presentation::kMotionTrailCapacity);

    const auto fading = presentation.compose({}, 1.0F, 0, {}, clock_time(310ms));
    REQUIRE(fading.circles.size() == dots::presentation::kMotionTrailCapacity + 1);
    CHECK(fading.circles.front().kind == dots::presentation::CircleKind::StructuralFade);
    CHECK(presentation.statistics().structural_fade_count == 1);

    const auto gone = presentation.compose({}, 1.0F, 0, {}, clock_time(411ms));
    CHECK(std::ranges::none_of(gone.circles, [](const auto& circle) {
        return circle.kind == dots::presentation::CircleKind::StructuralFade;
    }));
    CHECK(presentation.statistics().structural_fade_count == 0);
}

TEST_CASE("Dots presentation extracts live food before players", "[dots][presentation]") {
    dots::simulation::World world;
    const auto player = world.spawn_player(dots::simulation::PlayerOwnerId{0}, {2.0F, 3.0F});
    const auto food = world.spawn_food({8.0F, -4.0F});
    REQUIRE(player.has_value());
    REQUIRE(food.has_value());

    const auto frame = dots::presentation::extract_frame(world, {1.0F, -2.0F});

    REQUIRE(frame.camera == mycore::math::Vector2{1.0F, -2.0F});
    REQUIRE(frame.circles.size() == 2);
    REQUIRE(frame.circles[0].kind == dots::presentation::CircleKind::Food);
    REQUIRE(frame.circles[0].position == mycore::math::Vector2{8.0F, -4.0F});
    REQUIRE(frame.circles[0].mass == dots::simulation::kFoodMass);
    REQUIRE(frame.circles[0].radius ==
            dots::simulation::radius_for_mass(dots::simulation::kFoodMass));
    REQUIRE(frame.circles[1].kind == dots::presentation::CircleKind::Player);
    REQUIRE(frame.circles[1].owner_id == dots::protocol::PlayerOwnerId{0});
    REQUIRE(frame.circles[1].position == mycore::math::Vector2{2.0F, 3.0F});
    REQUIRE(frame.circles[1].mass == dots::simulation::kInitialPlayerMass);
    REQUIRE(frame.circles[1].radius ==
            dots::simulation::radius_for_mass(dots::simulation::kInitialPlayerMass));
}

TEST_CASE("Dots presentation extraction follows removals without stale IDs",
          "[dots][presentation]") {
    dots::simulation::World world;
    const auto removed_player = world.spawn_player(dots::simulation::PlayerOwnerId{0});
    const auto live_player = world.spawn_player(dots::simulation::PlayerOwnerId{1}, {4.0F, 5.0F});
    const auto removed_food = world.spawn_food({8.0F, 9.0F});
    const auto live_food = world.spawn_food({10.0F, 11.0F});
    REQUIRE(removed_player.has_value());
    REQUIRE(live_player.has_value());
    REQUIRE(removed_food.has_value());
    REQUIRE(live_food.has_value());
    REQUIRE(world.remove_player(*removed_player));
    REQUIRE(world.remove_food(*removed_food));

    const auto frame = dots::presentation::extract_frame(world, {});

    REQUIRE(frame.circles.size() == 2);
    REQUIRE(frame.circles[0].kind == dots::presentation::CircleKind::Food);
    REQUIRE(frame.circles[1].kind == dots::presentation::CircleKind::Player);
    REQUIRE(world.player_count() == 1);
    REQUIRE(world.food_count() == 1);
}

TEST_CASE("Dots presentation extracts an empty world", "[dots][presentation]") {
    const dots::simulation::World world;
    const auto frame = dots::presentation::extract_frame(world, {3.0F, 7.0F});

    REQUIRE(frame.camera == mycore::math::Vector2{3.0F, 7.0F});
    REQUIRE(frame.circles.empty());
}

TEST_CASE("Remote endpoint diagnostics include a visible interpolation connector",
          "[dots][presentation][remote]") {
    dots::replication::ReplicatedWorld world;
    REQUIRE(world.apply({
                .snapshot_id = dots::protocol::SnapshotId{1},
                .recipient = playing_session(dots::protocol::EntityId{1}),
                .owners = {{
                    .owner_id = dots::protocol::PlayerOwnerId{4},
                }},
                .entities =
                    {
                        {
                            .entity_id = dots::protocol::EntityId{1},
                            .kind = dots::protocol::EntityKind::Player,
                            .owner_id = dots::protocol::PlayerOwnerId{4},
                            .position_x = 0.0F,
                            .position_y = 0.0F,
                            .mass = 16.0F,
                        },
                    },
            }) == dots::replication::SnapshotApplyResult::Applied);

    const dots::presentation::RemotePresentationFrame remotes{
        .entities =
            {
                {
                    .entity_id = dots::protocol::EntityId{2},
                    .kind = dots::protocol::EntityKind::Player,
                    .position = {3.0F, 0.0F},
                    .mass = 16.0F,
                },
            },
    };
    const std::array endpoints{dots::presentation::RemoteEntityEndpoints{
        .older =
            dots::presentation::RemoteEntitySample{
                .entity_id = dots::protocol::EntityId{2},
                .kind = dots::protocol::EntityKind::Player,
                .position = {0.0F, 0.0F},
                .mass = 16.0F,
            },
        .newer =
            dots::presentation::RemoteEntitySample{
                .entity_id = dots::protocol::EntityId{2},
                .kind = dots::protocol::EntityKind::Player,
                .position = {4.0F, 0.0F},
                .mass = 16.0F,
            },
    }};

    const auto frame = dots::presentation::extract_remote_interpolated_predicted_frame(
        world,
        remotes,
        endpoints,
        {
            .entity_id = dots::protocol::EntityId{1},
            .presentation_position = {0.0F, 0.0F},
            .predicted_position = {0.0F, 0.0F},
            .show_prediction_layers = false,
        });

    REQUIRE(frame.circles.size() == 7);
    CHECK(frame.circles[2].kind == dots::presentation::CircleKind::RemoteOlderEndpointGhost);
    CHECK(frame.circles[3].kind == dots::presentation::CircleKind::RemoteNewerEndpointGhost);
    CHECK(frame.circles[4].kind ==
          dots::presentation::CircleKind::RemoteInterpolationConnectorStart);
    CHECK(frame.circles[5].kind ==
          dots::presentation::CircleKind::RemoteInterpolationConnectorMiddle);
    CHECK(frame.circles[6].kind == dots::presentation::CircleKind::RemoteInterpolationConnectorEnd);
    CHECK(frame.circles[4].position == mycore::math::Vector2{1.0F, 0.0F});
    CHECK(frame.circles[5].position == mycore::math::Vector2{2.0F, 0.0F});
    CHECK(frame.circles[6].position == mycore::math::Vector2{3.0F, 0.0F});

    const auto draw_list = dots::presentation::build_draw_list(frame, {});
    CHECK(draw_list.circles[4].radius == Catch::Approx(0.075F));
    CHECK(draw_list.circles[4].color == mycore::render::Color{0.0F, 0.9F, 0.95F, 0.95F});
    CHECK(draw_list.circles[6].color == mycore::render::Color{0.2F, 0.45F, 1.0F, 0.95F});
}

TEST_CASE("Predicted interaction scope replaces duplicate interpolated entities",
          "[dots][presentation][prediction]") {
    const auto owner = dots::protocol::PlayerOwnerId{4};
    dots::replication::ReplicatedWorld authority;
    REQUIRE(authority.apply({
                .snapshot_id = dots::protocol::SnapshotId{1},
                .recipient =
                    {
                        .mode = dots::protocol::SessionMode::Playing,
                        .owned_entity_ids =
                            {
                                dots::protocol::EntityId{1},
                                dots::protocol::EntityId{2},
                            },
                        .primary_entity_id = dots::protocol::EntityId{1},
                    },
                .owners = {{.owner_id = owner}},
                .entities =
                    {
                        {
                            .entity_id = dots::protocol::EntityId{1},
                            .kind = dots::protocol::EntityKind::Player,
                            .owner_id = owner,
                            .mass = 8.0F,
                        },
                        {
                            .entity_id = dots::protocol::EntityId{2},
                            .kind = dots::protocol::EntityKind::Player,
                            .owner_id = owner,
                            .position_x = 1.0F,
                            .mass = 8.0F,
                        },
                    },
            }) == dots::replication::SnapshotApplyResult::Applied);

    dots::simulation::World predicted;
    REQUIRE_FALSE(predicted.restore({
        .rules = {},
        .tick = mycore::time::Tick{1},
        .next_entity_id = 4,
        .owners = {{
            .owner_id = dots::simulation::PlayerOwnerId{owner.value()},
            .player_ids =
                {
                    dots::simulation::EntityId{1},
                    dots::simulation::EntityId{2},
                },
        }},
        .players =
            {
                {
                    .entity_id = dots::simulation::EntityId{1},
                    .owner_id = dots::simulation::PlayerOwnerId{owner.value()},
                    .position = {1.0F, 0.0F},
                    .mass = 8.0F,
                },
                {
                    .entity_id = dots::simulation::EntityId{2},
                    .owner_id = dots::simulation::PlayerOwnerId{owner.value()},
                    .position = {2.0F, 0.0F},
                    .mass = 8.0F,
                },
            },
        .food = {{
            .entity_id = dots::simulation::EntityId{3},
            .position = {3.0F, 0.0F},
        }},
    }));

    const dots::presentation::RemotePresentationFrame remotes{
        .entities =
            {
                {
                    .entity_id = dots::protocol::EntityId{2},
                    .kind = dots::protocol::EntityKind::Player,
                    .position = {20.0F, 0.0F},
                    .mass = 8.0F,
                },
                {
                    .entity_id = dots::protocol::EntityId{3},
                    .kind = dots::protocol::EntityKind::Food,
                    .position = {30.0F, 0.0F},
                    .mass = 1.0F,
                },
                {
                    .entity_id = dots::protocol::EntityId{4},
                    .kind = dots::protocol::EntityKind::Player,
                    .position = {4.0F, 0.0F},
                    .mass = 16.0F,
                },
                {
                    .entity_id = dots::protocol::EntityId{5},
                    .kind = dots::protocol::EntityKind::Player,
                    .position = {50.0F, 0.0F},
                    .mass = 16.0F,
                },
            },
    };
    const std::array predicted_scope{
        dots::protocol::EntityId{1},
        dots::protocol::EntityId{2},
        dots::protocol::EntityId{3},
        dots::protocol::EntityId{5},
    };
    const std::array endpoints{dots::presentation::RemoteEntityEndpoints{
        .older =
            dots::presentation::RemoteEntitySample{
                .entity_id = dots::protocol::EntityId{2},
                .kind = dots::protocol::EntityKind::Player,
                .position = {10.0F, 0.0F},
                .mass = 8.0F,
            },
        .newer =
            dots::presentation::RemoteEntitySample{
                .entity_id = dots::protocol::EntityId{2},
                .kind = dots::protocol::EntityKind::Player,
                .position = {20.0F, 0.0F},
                .mass = 8.0F,
            },
    }};

    const auto frame = dots::presentation::extract_remote_interpolated_predicted_frame(
        authority,
        predicted,
        predicted_scope,
        remotes,
        endpoints,
        {
            .entity_id = dots::protocol::EntityId{1},
            .presentation_position = {10.0F, 0.0F},
            .predicted_position = {1.0F, 0.0F},
            .show_prediction_layers = false,
            .show_replay_path = false,
        });

    REQUIRE(frame.circles.size() == 9);
    CHECK(frame.camera == mycore::math::Vector2{10.0F, 0.0F});
    CHECK(frame.circles[0].entity_id == dots::protocol::EntityId{4});
    CHECK(frame.circles[1].entity_id == dots::protocol::EntityId{3});
    CHECK(frame.circles[1].position == mycore::math::Vector2{3.0F, 0.0F});
    CHECK(frame.circles[2].entity_id == dots::protocol::EntityId{1});
    CHECK(frame.circles[2].position == mycore::math::Vector2{10.0F, 0.0F});
    CHECK(frame.circles[3].entity_id == dots::protocol::EntityId{2});
    CHECK(frame.circles[3].position == mycore::math::Vector2{2.0F, 0.0F});
    CHECK(frame.circles[4].kind == dots::presentation::CircleKind::RemoteOlderEndpointGhost);
    CHECK(frame.circles[5].kind == dots::presentation::CircleKind::RemoteNewerEndpointGhost);
    CHECK(frame.circles[4].position == mycore::math::Vector2{10.0F, 0.0F});
    CHECK(frame.circles[5].position == mycore::math::Vector2{20.0F, 0.0F});
}

TEST_CASE("Spectator camera follows the same interpolated sample used for drawing",
          "[dots][presentation][spectator]") {
    const dots::protocol::EntityId killer{9};
    const dots::presentation::RemotePresentationFrame remotes{
        .entities =
            {
                {
                    .entity_id = killer,
                    .kind = dots::protocol::EntityKind::Player,
                    .position = {4.0F, -3.0F},
                    .mass = 25.0F,
                },
                {
                    .entity_id = dots::protocol::EntityId{10},
                    .kind = dots::protocol::EntityKind::Food,
                    .position = {8.0F, 2.0F},
                    .mass = 1.0F,
                },
            },
    };
    dots::presentation::SpectatorCamera camera;
    camera.enter({100.0F, 100.0F}, 20.0F, killer, remotes);

    CHECK(camera.mode() == dots::presentation::SpectatorCameraMode::FollowKiller);
    CHECK(camera.position() == mycore::math::Vector2{4.0F, -3.0F});
    const auto frame = dots::presentation::extract_remote_interpolated_spectator_frame(
        remotes, {}, camera.position());
    const auto killer_circle =
        std::find_if(frame.circles.begin(), frame.circles.end(), [killer](const auto& circle) {
            return circle.entity_id == killer;
        });
    REQUIRE(killer_circle != frame.circles.end());
    CHECK(killer_circle->position == frame.camera);
}

TEST_CASE("Spectator camera supports bounded zoom, free pan, and confirmed follow toggle",
          "[dots][presentation][spectator]") {
    const dots::protocol::EntityId killer{9};
    const dots::presentation::RemotePresentationFrame remotes{
        .entities =
            {
                {
                    .entity_id = killer,
                    .kind = dots::protocol::EntityKind::Player,
                    .position = {4.0F, -3.0F},
                    .mass = 25.0F,
                },
            },
    };
    dots::presentation::SpectatorCamera camera;
    camera.enter({}, 20.0F, killer, remotes);
    camera.update(remotes, killer, {.toggle_follow = true}, 0.0F);
    REQUIRE(camera.mode() == dots::presentation::SpectatorCameraMode::Free);

    camera.update(remotes, killer, {.pan = {1.0F, 0.0F}, .zoom_steps = 100}, 0.5F);
    CHECK(camera.position() == mycore::math::Vector2{10.0F, -3.0F});
    CHECK(camera.pixels_per_world_unit() == 80.0F);

    camera.update(remotes, killer, {.zoom_steps = -100, .toggle_follow = true}, 0.0F);
    CHECK(camera.mode() == dots::presentation::SpectatorCameraMode::FollowKiller);
    CHECK(camera.position() == mycore::math::Vector2{4.0F, -3.0F});
    CHECK(camera.pixels_per_world_unit() == 5.0F);
}

TEST_CASE("Spectator camera keeps its last valid position when the confirmed target disappears",
          "[dots][presentation][spectator]") {
    const dots::protocol::EntityId killer{9};
    const dots::presentation::RemotePresentationFrame visible{
        .entities =
            {
                {
                    .entity_id = killer,
                    .kind = dots::protocol::EntityKind::Player,
                    .position = {4.0F, -3.0F},
                    .mass = 25.0F,
                },
            },
    };
    dots::presentation::SpectatorCamera camera;
    camera.enter({}, 20.0F, killer, visible);

    camera.update({}, killer, {}, 1.0F);
    CHECK(camera.mode() == dots::presentation::SpectatorCameraMode::Free);
    CHECK(camera.position() == mycore::math::Vector2{4.0F, -3.0F});

    camera.update({}, killer, {.toggle_follow = true}, 0.0F);
    CHECK(camera.mode() == dots::presentation::SpectatorCameraMode::Free);
    CHECK(camera.position() == mycore::math::Vector2{4.0F, -3.0F});
}

TEST_CASE("Spectator camera waits for its first confirmed follow sample",
          "[dots][presentation][spectator]") {
    const dots::protocol::EntityId killer{9};
    dots::presentation::SpectatorCamera camera;
    camera.enter({2.0F, 3.0F}, 20.0F, killer, {});

    CHECK(camera.mode() == dots::presentation::SpectatorCameraMode::FollowKiller);
    CHECK(camera.position() == mycore::math::Vector2{2.0F, 3.0F});

    camera.update(
        {
            .entities =
                {
                    {
                        .entity_id = killer,
                        .kind = dots::protocol::EntityKind::Player,
                        .position = {7.0F, 8.0F},
                        .mass = 25.0F,
                    },
                },
        },
        killer,
        {},
        0.0F);
    CHECK(camera.mode() == dots::presentation::SpectatorCameraMode::FollowKiller);
    CHECK(camera.position() == mycore::math::Vector2{7.0F, 8.0F});
}

TEST_CASE("Dots presentation extracts replicated state around its controlled player",
          "[dots][presentation][replication]") {
    dots::replication::ReplicatedWorld world;
    REQUIRE(world.apply({
                .snapshot_id = dots::protocol::SnapshotId{1},
                .recipient = playing_session(dots::protocol::EntityId{3}),
                .owners = {{
                    .owner_id = dots::protocol::PlayerOwnerId{4},
                }},
                .entities =
                    {
                        {
                            .entity_id = dots::protocol::EntityId{3},
                            .kind = dots::protocol::EntityKind::Player,
                            .owner_id = dots::protocol::PlayerOwnerId{4},
                            .position_x = 5.0F,
                            .position_y = -2.0F,
                            .mass = 16.0F,
                        },
                        {
                            .entity_id = dots::protocol::EntityId{7},
                            .kind = dots::protocol::EntityKind::Food,
                            .position_x = 8.0F,
                            .position_y = 4.0F,
                            .mass = 1.0F,
                        },
                    },
            }) == dots::replication::SnapshotApplyResult::Applied);

    const auto frame =
        dots::presentation::extract_replicated_frame(world, dots::protocol::EntityId{3});
    REQUIRE(frame.camera == mycore::math::Vector2{5.0F, -2.0F});
    REQUIRE(frame.circles.size() == 2);
    CHECK(frame.circles[0].kind == dots::presentation::CircleKind::Player);
    CHECK(frame.circles[0].radius == 4.0F);
    CHECK(frame.circles[1].kind == dots::presentation::CircleKind::Food);
    CHECK(frame.circles[1].radius == 1.0F);
}

TEST_CASE("Dots presentation keeps an interpolated follow target aligned with its camera",
          "[dots][presentation]") {
    dots::simulation::World world;
    const auto player = world.spawn_player(dots::simulation::PlayerOwnerId{0}, {4.0F, 5.0F});
    const auto food = world.spawn_food({8.0F, 9.0F});
    REQUIRE(player.has_value());
    REQUIRE(food.has_value());
    const auto frame = dots::presentation::extract_interpolated_follow_frame(
        world,
        {
            .entity_id = *player,
            .previous_position = {0.0F, 1.0F},
            .current_position = {4.0F, 5.0F},
            .alpha = 0.5F,
            .show_current_position_ghost = true,
        });

    REQUIRE(frame.circles.size() == 3);
    REQUIRE(frame.circles[0].position == mycore::math::Vector2{8.0F, 9.0F});
    REQUIRE(frame.circles[1].position == frame.camera);
    REQUIRE(frame.circles[2].position == mycore::math::Vector2{4.0F, 5.0F});
    REQUIRE(frame.circles[2].kind == dots::presentation::CircleKind::PositionGhost);

    const auto draw_list = dots::presentation::build_draw_list(frame, {});
    REQUIRE(draw_list.circles[2].center == frame.circles[2].position);
    REQUIRE(draw_list.circles[2].color.alpha == 0.0F);
    REQUIRE(draw_list.circles[2].outline_color == mycore::render::Color{1.0F, 1.0F, 1.0F, 0.9F});
    REQUIRE(draw_list.circles[2].outline_width_pixels == 2.0F);

    const auto fixed_frame =
        dots::presentation::extract_interpolated_follow_frame(world,
                                                              {
                                                                  .entity_id = *player,
                                                                  .previous_position = {0.0F, 1.0F},
                                                                  .current_position = {4.0F, 5.0F},
                                                                  .alpha = 1.0F,
                                                              });
    REQUIRE(fixed_frame.camera == mycore::math::Vector2{4.0F, 5.0F});
    REQUIRE(fixed_frame.circles.size() == 2);
    REQUIRE(fixed_frame.circles[1].position == fixed_frame.camera);

    REQUIRE_THROWS(
        dots::presentation::extract_interpolated_follow_frame(world,
                                                              {
                                                                  .entity_id = *player,
                                                                  .previous_position = {},
                                                                  .current_position = {},
                                                                  .alpha = 1.5F,
                                                              }));
}

TEST_CASE("Dots presentation maps game meaning to a Render2D draw list", "[dots][presentation]") {
    const dots::presentation::FrameData frame{
        .camera = {4.0F, -7.0F},
        .circles =
            {
                {
                    .position = {1.0F, 2.0F},
                    .mass = dots::simulation::kFoodMass,
                    .radius = 0.5F,
                    .kind = dots::presentation::CircleKind::Food,
                },
                {
                    .position = {-2.0F, 6.0F},
                    .mass = dots::simulation::kInitialPlayerMass,
                    .radius = 1.5F,
                    .kind = dots::presentation::CircleKind::Player,
                },
            },
    };
    const dots::presentation::Settings settings{
        .pixels_per_world_unit = 32.0F,
        .draw_grid = true,
        .grid_spacing_world_units = 5.0F,
        .background = {0.1F, 0.2F, 0.3F, 1.0F},
        .grid = {0.2F, 0.3F, 0.4F, 1.0F},
        .player = {0.3F, 0.4F, 0.5F, 1.0F},
        .player_growth = {0.9F, 0.8F, 0.1F, 1.0F},
        .food = {0.4F, 0.5F, 0.6F, 1.0F},
    };

    const auto draw_list = dots::presentation::build_draw_list(frame, settings);

    REQUIRE(draw_list.camera.center == frame.camera);
    REQUIRE(draw_list.camera.pixels_per_world_unit == 32.0F);
    REQUIRE(draw_list.clear_color == settings.background);
    REQUIRE(draw_list.grid.has_value());
    REQUIRE(draw_list.grid->spacing_world_units == 5.0F);
    REQUIRE(draw_list.grid->color == settings.grid);
    REQUIRE(draw_list.circles.size() == 2);
    REQUIRE(draw_list.circles[0] == mycore::render_2d::Circle{
                                        .center = {1.0F, 2.0F},
                                        .radius = 0.5F,
                                        .color = settings.food,
                                    });
    REQUIRE(draw_list.circles[1] == mycore::render_2d::Circle{
                                        .center = {-2.0F, 6.0F},
                                        .radius = 1.5F,
                                        .color = settings.player,
                                    });
}

TEST_CASE("Dots presentation can omit the Render2D grid", "[dots][presentation]") {
    const dots::presentation::FrameData frame{};
    const dots::presentation::Settings settings{.draw_grid = false};

    const auto draw_list = dots::presentation::build_draw_list(frame, settings);

    REQUIRE_FALSE(draw_list.grid.has_value());
}

TEST_CASE("Dots presentation assigns one stable color to every piece of an owner",
          "[dots][presentation]") {
    const dots::presentation::FrameData frame{
        .circles =
            {
                {
                    .mass = dots::simulation::kInitialPlayerMass,
                    .radius = 4.0F,
                    .kind = dots::presentation::CircleKind::Player,
                    .entity_id = dots::protocol::EntityId{1},
                    .owner_id = dots::protocol::PlayerOwnerId{4},
                },
                {
                    .mass =
                        dots::simulation::kInitialPlayerMass + (4.0F * dots::simulation::kFoodMass),
                    .radius = 5.0F,
                    .kind = dots::presentation::CircleKind::Player,
                    .entity_id = dots::protocol::EntityId{2},
                    .owner_id = dots::protocol::PlayerOwnerId{4},
                },
                {
                    .mass =
                        dots::simulation::kInitialPlayerMass + (8.0F * dots::simulation::kFoodMass),
                    .radius = 6.0F,
                    .kind = dots::presentation::CircleKind::Player,
                    .entity_id = dots::protocol::EntityId{3},
                    .owner_id = dots::protocol::PlayerOwnerId{5},
                },
            },
    };
    const dots::presentation::Settings settings{
        .player = {0.0F, 0.2F, 1.0F, 1.0F},
        .player_growth = {1.0F, 0.8F, 0.0F, 1.0F},
    };

    const auto draw_list = dots::presentation::build_draw_list(frame, settings);

    REQUIRE(draw_list.circles[0].color == draw_list.circles[1].color);
    REQUIRE(draw_list.circles[0].color != draw_list.circles[2].color);
}

TEST_CASE("Prediction diagnostics outline every confirmed owned split piece",
          "[dots][presentation][prediction]") {
    const auto owner_id = dots::protocol::PlayerOwnerId{4};
    dots::replication::ReplicatedWorld world;
    REQUIRE(world.apply({
                .snapshot_id = dots::protocol::SnapshotId{2},
                .recipient =
                    {
                        .mode = dots::protocol::SessionMode::Playing,
                        .owned_entity_ids =
                            {
                                dots::protocol::EntityId{3},
                                dots::protocol::EntityId{4},
                            },
                        .primary_entity_id = dots::protocol::EntityId{3},
                    },
                .owners = {{.owner_id = owner_id}},
                .entities =
                    {
                        {
                            .entity_id = dots::protocol::EntityId{3},
                            .kind = dots::protocol::EntityKind::Player,
                            .owner_id = owner_id,
                            .position_x = 5.0F,
                            .mass = 8.0F,
                        },
                        {
                            .entity_id = dots::protocol::EntityId{4},
                            .kind = dots::protocol::EntityKind::Player,
                            .owner_id = owner_id,
                            .position_x = 7.0F,
                            .mass = 8.0F,
                        },
                    },
            }) == dots::replication::SnapshotApplyResult::Applied);

    const auto frame = dots::presentation::extract_predicted_replicated_frame(
        world,
        {
            .entity_id = dots::protocol::EntityId{3},
            .presentation_position = {5.5F, 0.0F},
            .predicted_position = {5.25F, 0.0F},
            .show_replay_path = false,
        });

    const auto authoritative_count =
        std::ranges::count(frame.circles,
                           dots::presentation::CircleKind::AuthoritativeSampleGhost,
                           &dots::presentation::CircleInstance::kind);
    CHECK(authoritative_count == 2);
    CHECK(std::ranges::any_of(frame.circles, [](const auto& circle) {
        return circle.kind == dots::presentation::CircleKind::AuthoritativeSampleGhost &&
               circle.entity_id == dots::protocol::EntityId{3};
    }));
    CHECK(std::ranges::any_of(frame.circles, [](const auto& circle) {
        return circle.kind == dots::presentation::CircleKind::AuthoritativeSampleGhost &&
               circle.entity_id == dots::protocol::EntityId{4};
    }));
}

TEST_CASE("Local prediction presentation preserves continuity and decays over 100 ms",
          "[dots][presentation][prediction]") {
    using namespace std::chrono_literals;
    dots::presentation::LocalPredictionPresentation presentation;
    const std::array replay_path{mycore::math::Vector2{0.25F, 0.0F},
                                 mycore::math::Vector2{0.5F, 0.0F}};
    presentation.update({.predicted_position = {1.0F, 0.0F}}, clock_time(0ms));

    const dots::presentation::LocalPredictionSample correction{
        .predicted_position = {},
        .accumulated_correction_displacement = {1.0F, 0.0F},
        .correction_sequence = 1,
        .pre_correction_position = mycore::math::Vector2{1.0F, 0.0F},
        .correction_replay_path = replay_path,
    };
    presentation.update(correction, clock_time(10ms));
    CHECK(presentation.predicted_position() == mycore::math::Vector2{});
    CHECK(presentation.presentation_position() == mycore::math::Vector2{1.0F, 0.0F});
    CHECK(presentation.smoothing_offset() == mycore::math::Vector2{1.0F, 0.0F});
    CHECK(presentation.correction_visual_active());
    CHECK(presentation.retained_pre_correction_position() ==
          std::optional{mycore::math::Vector2{1.0F, 0.0F}});
    CHECK(presentation.retained_correction_replay_path().size() == 2);

    presentation.update(correction, clock_time(60ms));
    CHECK(presentation.presentation_position().x == Catch::Approx(0.5F));
    CHECK(presentation.smoothing_offset().x == Catch::Approx(0.5F));

    presentation.update(correction, clock_time(110ms));
    CHECK(presentation.presentation_position() == mycore::math::Vector2{});
    CHECK(presentation.smoothing_offset() == mycore::math::Vector2{});
    CHECK(presentation.correction_visual_active());

    presentation.update(correction, clock_time(2010ms));
    CHECK_FALSE(presentation.correction_visual_active());
    CHECK_FALSE(presentation.retained_pre_correction_position().has_value());
    CHECK(presentation.retained_correction_replay_path().empty());
}

TEST_CASE("Overlapping corrections compound residuals and hard resync clears presentation",
          "[dots][presentation][prediction]") {
    using namespace std::chrono_literals;
    dots::presentation::LocalPredictionPresentation presentation;
    presentation.update({.predicted_position = {2.0F, 0.0F}}, clock_time(0ms));
    presentation.update(
        {
            .predicted_position = {1.0F, 0.0F},
            .accumulated_correction_displacement = {1.0F, 0.0F},
            .correction_sequence = 1,
            .pre_correction_position = mycore::math::Vector2{2.0F, 0.0F},
        },
        clock_time(0ms));
    CHECK(presentation.presentation_position().x == Catch::Approx(2.0F));
    CHECK(presentation.correction_visual_active());
    presentation.clear_correction_visuals();
    CHECK_FALSE(presentation.correction_visual_active());

    presentation.update(
        {
            .predicted_position = {1.0F, 0.0F},
            .accumulated_correction_displacement = {1.0F, 0.0F},
            .correction_sequence = 1,
        },
        clock_time(50ms));
    CHECK(presentation.presentation_position().x == Catch::Approx(1.5F));
    CHECK_FALSE(presentation.correction_visual_active());
    presentation.update(
        {
            .predicted_position = {},
            .accumulated_correction_displacement = {2.0F, 0.0F},
            .correction_sequence = 2,
            .pre_correction_position = mycore::math::Vector2{1.0F, 0.0F},
        },
        clock_time(50ms));
    CHECK(presentation.presentation_position().x == Catch::Approx(1.5F));
    CHECK(presentation.smoothing_offset().x == Catch::Approx(1.5F));
    CHECK(presentation.correction_visual_active());

    presentation.update(
        {
            .predicted_position = {4.0F, 2.0F},
            .hard_resync_sequence = 1,
        },
        clock_time(60ms));
    CHECK(presentation.presentation_position() == mycore::math::Vector2{4.0F, 2.0F});
    CHECK(presentation.smoothing_offset() == mycore::math::Vector2{});
    CHECK_FALSE(presentation.correction_visual_active());
}

TEST_CASE("Prediction correction history is bounded and fades stable semantic colors",
          "[dots][presentation][prediction][debug]") {
    using namespace std::chrono_literals;
    dots::presentation::PredictionCorrectionHistory history{2};
    const std::array first{
        dots::presentation::PredictionCorrectionSample{
            .sequence = 1,
            .entity_id = dots::protocol::EntityId{3},
            .pre_correction_position = {1.0F, 0.0F},
            .mass = 16.0F,
        },
    };
    history.update(first, 0, clock_time(0ms));
    const std::array second{
        first.front(),
        dots::presentation::PredictionCorrectionSample{
            .sequence = 2,
            .entity_id = dots::protocol::EntityId{7},
            .pre_correction_position = {2.0F, 0.0F},
            .mass = 9.0F,
            .remote = true,
        },
    };
    history.update(second, 0, clock_time(500ms));
    const std::array third{
        first.front(),
        second.back(),
        dots::presentation::PredictionCorrectionSample{
            .sequence = 3,
            .entity_id = dots::protocol::EntityId{8},
            .pre_correction_position = {3.0F, 0.0F},
            .mass = 4.0F,
            .remote = true,
        },
    };
    history.update(third, 0, clock_time(1000ms));

    REQUIRE(history.size() == 2);
    CHECK(history.capacity() == 2);
    CHECK(history.local_count() == 0);
    CHECK(history.remote_count() == 2);
    CHECK(history.ghosts()[0].entity_id == dots::protocol::EntityId{7});
    CHECK(history.ghosts()[0].opacity == Catch::Approx(0.8F));
    CHECK(history.ghosts()[1].entity_id == dots::protocol::EntityId{8});
    CHECK(history.ghosts()[1].opacity == Catch::Approx(1.0F));

    history.clear();
    history.update(third, 0, clock_time(1100ms));
    CHECK(history.size() == 0);

    const std::array fourth{
        dots::presentation::PredictionCorrectionSample{
            .sequence = 4,
            .entity_id = dots::protocol::EntityId{9},
            .pre_correction_position = {4.0F, 0.0F},
            .mass = 25.0F,
        },
    };
    history.update(fourth, 0, clock_time(1200ms));
    REQUIRE(history.size() == 1);
    history.update(fourth, 0, clock_time(3200ms));
    CHECK(history.size() == 0);
}

TEST_CASE("A correction observed after a hard resync starts from the reset presentation",
          "[dots][presentation][prediction]") {
    using namespace std::chrono_literals;
    dots::presentation::LocalPredictionPresentation presentation;
    presentation.update(
        {
            .predicted_position = {2.0F, 0.0F},
            .accumulated_correction_displacement = {1.0F, 0.0F},
            .correction_sequence = 1,
        },
        clock_time(0ms));
    presentation.update(
        {
            .predicted_position = {3.0F, 0.0F},
            .accumulated_correction_displacement = {1.0F, 0.0F},
            .correction_sequence = 1,
            .hard_resync_sequence = 1,
            .pre_correction_position = mycore::math::Vector2{4.0F, 0.0F},
        },
        clock_time(10ms));

    CHECK(presentation.predicted_position() == mycore::math::Vector2{3.0F, 0.0F});
    CHECK(presentation.presentation_position() == mycore::math::Vector2{4.0F, 0.0F});
    CHECK(presentation.smoothing_offset() == mycore::math::Vector2{1.0F, 0.0F});
    CHECK(presentation.correction_visual_active());
}

TEST_CASE("Predicted replicated extraction separates presentation and known state layers",
          "[dots][presentation][prediction]") {
    dots::replication::ReplicatedWorld world;
    REQUIRE(world.apply({
                .snapshot_id = dots::protocol::SnapshotId{1},
                .recipient = playing_session(dots::protocol::EntityId{3}),
                .owners = {{
                    .owner_id = dots::protocol::PlayerOwnerId{4},
                }},
                .entities =
                    {
                        {
                            .entity_id = dots::protocol::EntityId{3},
                            .kind = dots::protocol::EntityKind::Player,
                            .owner_id = dots::protocol::PlayerOwnerId{4},
                            .position_x = 5.0F,
                            .position_y = -2.0F,
                            .mass = 16.0F,
                        },
                        {
                            .entity_id = dots::protocol::EntityId{7},
                            .kind = dots::protocol::EntityKind::Food,
                            .position_x = 8.0F,
                            .position_y = 4.0F,
                            .mass = 1.0F,
                        },
                    },
            }) == dots::replication::SnapshotApplyResult::Applied);
    const std::array replay_path{mycore::math::Vector2{5.25F, -1.75F},
                                 mycore::math::Vector2{5.5F, -1.5F}};
    const dots::presentation::PredictedReplicatedPlayer player{
        .entity_id = dots::protocol::EntityId{3},
        .presentation_position = {6.0F, -1.0F},
        .predicted_position = {5.5F, -1.5F},
        .pre_correction_position = mycore::math::Vector2{7.0F, 0.0F},
        .correction_replay_path = replay_path,
    };

    const auto frame = dots::presentation::extract_predicted_replicated_frame(world, player);
    REQUIRE(frame.camera == player.presentation_position);
    REQUIRE(frame.circles.size() == 7);
    CHECK(frame.circles[0].position == player.presentation_position);
    CHECK(frame.circles[2].kind == dots::presentation::CircleKind::PredictedPositionGhost);
    CHECK(frame.circles[2].position == player.predicted_position);
    CHECK(frame.circles[3].kind == dots::presentation::CircleKind::AuthoritativeSampleGhost);
    CHECK(frame.circles[3].position == mycore::math::Vector2{5.0F, -2.0F});
    CHECK(frame.circles[4].kind == dots::presentation::CircleKind::PreCorrectionGhost);
    CHECK(frame.circles[5].kind == dots::presentation::CircleKind::ReplayMarker);
    CHECK(frame.circles[6].kind == dots::presentation::CircleKind::ReplayMarker);

    const auto draw_list = dots::presentation::build_draw_list(frame, {});
    CHECK(draw_list.camera.center == player.presentation_position);
    CHECK(draw_list.circles[2].radius == Catch::Approx(4.05F));
    CHECK(draw_list.circles[3].radius == Catch::Approx(4.1F));
    CHECK(draw_list.circles[4].radius == Catch::Approx(4.15F));
    CHECK(draw_list.circles[5].radius == Catch::Approx(0.15F));
    CHECK(draw_list.circles[3].outline_color == mycore::render::Color{1.0F, 0.55F, 0.12F, 0.95F});
    CHECK(draw_list.circles[4].outline_color == mycore::render::Color{1.0F, 0.1F, 0.75F, 0.95F});

    const auto hidden = dots::presentation::extract_predicted_replicated_frame(
        world,
        {
            .entity_id = player.entity_id,
            .presentation_position = player.presentation_position,
            .predicted_position = player.predicted_position,
            .show_prediction_layers = false,
            .show_replay_path = false,
        });
    CHECK(hidden.circles.size() == 2);
}

TEST_CASE("Prediction correction ghosts retain entity geometry and fade outline opacity",
          "[dots][presentation][prediction][debug]") {
    dots::replication::ReplicatedWorld world;
    REQUIRE(world.apply({
                .snapshot_id = dots::protocol::SnapshotId{1},
                .recipient = playing_session(dots::protocol::EntityId{3}),
                .owners = {{
                    .owner_id = dots::protocol::PlayerOwnerId{4},
                }},
                .entities = {{
                    .entity_id = dots::protocol::EntityId{3},
                    .kind = dots::protocol::EntityKind::Player,
                    .owner_id = dots::protocol::PlayerOwnerId{4},
                    .mass = 16.0F,
                }},
            }) == dots::replication::SnapshotApplyResult::Applied);
    const std::array corrections{
        dots::presentation::PredictionCorrectionGhost{
            .entity_id = dots::protocol::EntityId{9},
            .position = {5.0F, 6.0F},
            .mass = 9.0F,
            .opacity = 0.25F,
            .remote = true,
        },
    };

    const auto frame = dots::presentation::extract_predicted_replicated_frame(
        world,
        {
            .entity_id = dots::protocol::EntityId{3},
            .presentation_position = {},
            .predicted_position = {},
            .correction_ghosts = corrections,
        });

    REQUIRE(frame.circles.size() == 4);
    CHECK(frame.circles.back().kind == dots::presentation::CircleKind::PreCorrectionGhost);
    CHECK(frame.circles.back().entity_id == dots::protocol::EntityId{9});
    CHECK(frame.circles.back().radius == Catch::Approx(3.0F));
    const auto draw_list = dots::presentation::build_draw_list(frame, {});
    CHECK(draw_list.circles.back().outline_color.alpha == Catch::Approx(0.2375F));
}

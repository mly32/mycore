#include "dots/presentation/presentation.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Dots presentation extracts live food before players", "[dots][presentation]") {
    dots::simulation::World world;
    const auto player = world.spawn_player({2.0F, 3.0F});
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
    REQUIRE(frame.circles[1].position == mycore::math::Vector2{2.0F, 3.0F});
    REQUIRE(frame.circles[1].mass == dots::simulation::kInitialPlayerMass);
    REQUIRE(frame.circles[1].radius ==
            dots::simulation::radius_for_mass(dots::simulation::kInitialPlayerMass));
}

TEST_CASE("Dots presentation extraction follows removals without stale IDs",
          "[dots][presentation]") {
    dots::simulation::World world;
    const auto removed_player = world.spawn_player();
    const auto live_player = world.spawn_player({4.0F, 5.0F});
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

TEST_CASE("Dots presentation extracts replicated state around its controlled player",
          "[dots][presentation][replication]") {
    dots::replication::ReplicatedWorld world;
    REQUIRE(world.apply({
                .snapshot_id = dots::protocol::SnapshotId{1},
                .entities =
                    {
                        {
                            .entity_id = dots::protocol::EntityId{3},
                            .kind = dots::protocol::EntityKind::Player,
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
    const auto player = world.spawn_player({4.0F, 5.0F});
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

TEST_CASE("Dots presentation shifts player color as food mass is gained", "[dots][presentation]") {
    const dots::presentation::FrameData frame{
        .circles =
            {
                {
                    .mass = dots::simulation::kInitialPlayerMass,
                    .radius = 4.0F,
                    .kind = dots::presentation::CircleKind::Player,
                },
                {
                    .mass =
                        dots::simulation::kInitialPlayerMass + (4.0F * dots::simulation::kFoodMass),
                    .radius = 5.0F,
                    .kind = dots::presentation::CircleKind::Player,
                },
                {
                    .mass =
                        dots::simulation::kInitialPlayerMass + (8.0F * dots::simulation::kFoodMass),
                    .radius = 6.0F,
                    .kind = dots::presentation::CircleKind::Player,
                },
            },
    };
    const dots::presentation::Settings settings{
        .player = {0.0F, 0.2F, 1.0F, 1.0F},
        .player_growth = {1.0F, 0.8F, 0.0F, 1.0F},
    };

    const auto draw_list = dots::presentation::build_draw_list(frame, settings);

    REQUIRE(draw_list.circles[0].color == settings.player);
    REQUIRE(draw_list.circles[1].color == mycore::render::Color{0.5F, 0.5F, 0.5F, 1.0F});
    REQUIRE(draw_list.circles[2].color == settings.player_growth);
}

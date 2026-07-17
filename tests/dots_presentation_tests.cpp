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
    REQUIRE(frame.circles[0].entity_id == *food);
    REQUIRE(frame.circles[0].kind == dots::presentation::CircleKind::Food);
    REQUIRE(frame.circles[0].position == mycore::math::Vector2{8.0F, -4.0F});
    REQUIRE(frame.circles[0].radius ==
            dots::simulation::radius_for_mass(dots::simulation::kFoodMass));
    REQUIRE(frame.circles[1].entity_id == *player);
    REQUIRE(frame.circles[1].kind == dots::presentation::CircleKind::Player);
    REQUIRE(frame.circles[1].position == mycore::math::Vector2{2.0F, 3.0F});
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
    REQUIRE(frame.circles[0].entity_id == *live_food);
    REQUIRE(frame.circles[1].entity_id == *live_player);
    REQUIRE(world.player_count() == 1);
    REQUIRE(world.food_count() == 1);
}

TEST_CASE("Dots presentation extracts an empty world", "[dots][presentation]") {
    const dots::simulation::World world;
    const auto frame = dots::presentation::extract_frame(world, {3.0F, 7.0F});

    REQUIRE(frame.camera == mycore::math::Vector2{3.0F, 7.0F});
    REQUIRE(frame.circles.empty());
}

#include "dots/simulation/world.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>

TEST_CASE("World spawns and removes players", "[dots][simulation]") {
    dots::simulation::World world;
    const auto first_result = world.spawn_player({1.0F, 2.0F});
    const auto second_result = world.spawn_player({3.0F, 4.0F});
    REQUIRE(first_result.has_value());
    REQUIRE(second_result.has_value());
    const auto first = *first_result;
    const auto second = *second_result;

    REQUIRE(first != second);
    REQUIRE(world.player_count() == 2);
    REQUIRE(world.contains(first));
    REQUIRE(world.position(first) == mycore::math::Vector2{1.0F, 2.0F});

    REQUIRE(world.remove_player(first));
    REQUIRE_FALSE(world.contains(first));
    REQUIRE(world.contains(second));
    REQUIRE(world.position(second) == mycore::math::Vector2{3.0F, 4.0F});
    REQUIRE(world.player_count() == 1);
    REQUIRE_FALSE(world.remove_player(first));
}

TEST_CASE("Movement input advances a player by one 30 Hz tick", "[dots][simulation]") {
    dots::simulation::World world;
    const auto player_result = world.spawn_player();
    REQUIRE(player_result.has_value());
    const auto player = *player_result;

    REQUIRE(world.apply_input({
        .id = dots::simulation::InputCommandId{0},
        .entity_id = player,
        .movement = {3.0F, 4.0F},
    }));
    REQUIRE(world.step());

    const auto position = world.position(player);
    REQUIRE(position.has_value());
    REQUIRE(position->x == Catch::Approx(0.12F));
    REQUIRE(position->y == Catch::Approx(0.16F));
    REQUIRE(world.tick() == mycore::time::Tick{1});
}

TEST_CASE("World applies persistent movement over multiple ticks", "[dots][simulation]") {
    dots::simulation::World world;
    const auto player_result = world.spawn_player({2.0F, -1.0F});
    REQUIRE(player_result.has_value());
    const auto player = *player_result;
    REQUIRE(world.apply_input({
        .id = dots::simulation::InputCommandId{4},
        .entity_id = player,
        .movement = {1.0F, 0.0F},
    }));

    for (std::uint32_t tick = 0; tick < dots::simulation::kTickRateHz; ++tick) {
        REQUIRE(world.step());
    }

    const auto position = world.position(player);
    REQUIRE(position.has_value());
    REQUIRE(position->x == Catch::Approx(8.0F));
    REQUIRE(position->y == Catch::Approx(-1.0F));
    REQUIRE(world.tick() == mycore::time::Tick{dots::simulation::kTickRateHz});
}

TEST_CASE("Recorded inputs replay to a deterministic final state", "[dots][simulation][replay]") {
    struct ReplayEntry {
        mycore::math::Vector2 movement;
        std::size_t tick_count;
    };

    constexpr std::array replay{
        ReplayEntry{.movement = {1.0F, 0.0F}, .tick_count = 5},
        ReplayEntry{.movement = {0.0F, 1.0F}, .tick_count = 3},
        ReplayEntry{.movement = {-1.0F, 0.0F}, .tick_count = 2},
    };

    dots::simulation::World world;
    const auto player_result = world.spawn_player();
    REQUIRE(player_result.has_value());
    const auto player = *player_result;
    std::uint32_t input_id = 0;

    for (const auto& entry : replay) {
        REQUIRE(world.apply_input({
            .id = dots::simulation::InputCommandId{input_id++},
            .entity_id = player,
            .movement = entry.movement,
        }));
        for (std::size_t tick = 0; tick < entry.tick_count; ++tick) {
            REQUIRE(world.step());
        }
    }

    const auto position = world.position(player);
    REQUIRE(position.has_value());
    REQUIRE(position->x == Catch::Approx(0.6F));
    REQUIRE(position->y == Catch::Approx(0.6F));
    REQUIRE(world.tick() == mycore::time::Tick{10});
}

TEST_CASE("World rejects invalid, unknown, and stale input", "[dots][simulation]") {
    dots::simulation::World world;
    const auto player_result = world.spawn_player();
    REQUIRE(player_result.has_value());
    const auto player = *player_result;

    REQUIRE_FALSE(world.apply_input({
        .id = dots::simulation::InputCommandId::invalid(),
        .entity_id = player,
        .movement = {1.0F, 0.0F},
    }));
    REQUIRE_FALSE(world.apply_input({
        .id = dots::simulation::InputCommandId{0},
        .entity_id = dots::simulation::EntityId{99},
        .movement = {1.0F, 0.0F},
    }));
    REQUIRE(world.apply_input({
        .id = dots::simulation::InputCommandId{2},
        .entity_id = player,
        .movement = {1.0F, 0.0F},
    }));
    REQUIRE_FALSE(world.apply_input({
        .id = dots::simulation::InputCommandId{1},
        .entity_id = player,
        .movement = {0.0F, 1.0F},
    }));
}

TEST_CASE("Player eats food and updates mass and radius", "[dots][simulation][food]") {
    dots::simulation::World world;
    const auto player_result = world.spawn_player();
    const auto food_result = world.spawn_food({5.0F, 0.0F});
    REQUIRE(player_result.has_value());
    REQUIRE(food_result.has_value());
    const auto player = *player_result;
    const auto food = *food_result;

    REQUIRE(world.food_count() == 1);
    REQUIRE(world.mass(player) == dots::simulation::kInitialPlayerMass);
    REQUIRE(world.radius(player) ==
            dots::simulation::radius_for_mass(dots::simulation::kInitialPlayerMass));
    REQUIRE(world.step());

    REQUIRE_FALSE(world.contains(food));
    REQUIRE(world.food_count() == 0);
    REQUIRE(world.mass(player) ==
            dots::simulation::kInitialPlayerMass + dots::simulation::kFoodMass);
    REQUIRE(world.radius(player).has_value());
    REQUIRE(*world.radius(player) == Catch::Approx(std::sqrt(dots::simulation::kInitialPlayerMass +
                                                             dots::simulation::kFoodMass)));
}

TEST_CASE("Invalid spawns do not consume entity IDs", "[dots][simulation][validation]") {
    dots::simulation::World world;
    const auto huge = std::numeric_limits<float>::max();

    REQUIRE_FALSE(world.spawn_food({huge, 0.0F}).has_value());
    const auto player = world.spawn_player();
    REQUIRE(player == dots::simulation::EntityId{0});
    REQUIRE_FALSE(world.contains(dots::simulation::EntityId{99}));
    REQUIRE_FALSE(world.position(dots::simulation::EntityId{99}).has_value());
    REQUIRE_FALSE(world.remove_food(dots::simulation::EntityId{99}));
}

TEST_CASE("Broad-phase candidates require exact overlap to be eaten", "[dots][simulation][food]") {
    dots::simulation::World world;
    REQUIRE(world.spawn_player().has_value());
    const auto food = world.spawn_food({7.5F, 0.0F});
    REQUIRE(food.has_value());

    REQUIRE(world.step());

    REQUIRE(world.contains(*food));
    REQUIRE(world.food_count() == 1);
}

TEST_CASE("Lowest entity ID wins contested food deterministically", "[dots][simulation][food]") {
    dots::simulation::World world;
    const auto removed = world.spawn_player();
    const auto lower_id = world.spawn_player();
    const auto higher_id = world.spawn_player();
    REQUIRE(removed.has_value());
    REQUIRE(lower_id.has_value());
    REQUIRE(higher_id.has_value());
    REQUIRE(world.remove_player(*removed));
    REQUIRE(world.spawn_food({0.0F, 0.0F}).has_value());

    REQUIRE(world.step());

    REQUIRE(world.food_count() == 0);
    REQUIRE(world.mass(*lower_id) ==
            dots::simulation::kInitialPlayerMass + dots::simulation::kFoodMass);
    REQUIRE(world.mass(*higher_id) == dots::simulation::kInitialPlayerMass);
}

TEST_CASE("Growth affects collisions on the following tick", "[dots][simulation][food]") {
    dots::simulation::World world;
    REQUIRE(world.spawn_player().has_value());
    REQUIRE(world.spawn_food({5.0F, 0.0F}).has_value());
    const auto farther_food = world.spawn_food({5.1F, 0.0F});
    REQUIRE(farther_food.has_value());

    REQUIRE(world.step());
    REQUIRE(world.food_count() == 1);
    REQUIRE(world.contains(*farther_food));

    REQUIRE(world.step());
    REQUIRE(world.food_count() == 0);
}

TEST_CASE("Moving replay consumes food in deterministic order",
          "[dots][simulation][food][replay]") {
    dots::simulation::World world;
    const auto player_result = world.spawn_player();
    REQUIRE(player_result.has_value());
    const auto player = *player_result;
    REQUIRE(world.spawn_food({5.4F, 0.0F}).has_value());
    REQUIRE(world.spawn_food({0.6F, 5.6F}).has_value());

    REQUIRE(world.apply_input({
        .id = dots::simulation::InputCommandId{0},
        .entity_id = player,
        .movement = {1.0F, 0.0F},
    }));
    for (std::size_t tick = 0; tick < 3; ++tick) {
        REQUIRE(world.step());
    }
    REQUIRE(world.food_count() == 1);

    REQUIRE(world.apply_input({
        .id = dots::simulation::InputCommandId{1},
        .entity_id = player,
        .movement = {0.0F, 1.0F},
    }));
    for (std::size_t tick = 0; tick < 3; ++tick) {
        REQUIRE(world.step());
    }

    REQUIRE(world.food_count() == 0);
    REQUIRE(world.mass(player) ==
            dots::simulation::kInitialPlayerMass + (2.0F * dots::simulation::kFoodMass));
    const auto position = world.position(player);
    REQUIRE(position.has_value());
    REQUIRE(position->x == Catch::Approx(0.6F));
    REQUIRE(position->y == Catch::Approx(0.6F));
}

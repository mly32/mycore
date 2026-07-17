#include "dots/simulation/world.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

TEST_CASE("World spawns and removes players", "[dots][simulation]") {
    dots::simulation::World world;
    const auto first = world.spawn_player({1.0F, 2.0F});
    const auto second = world.spawn_player({3.0F, 4.0F});

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
    const auto player = world.spawn_player();

    REQUIRE(world.apply_input({
        .id = dots::simulation::InputCommandId{0},
        .entity_id = player,
        .movement = {3.0F, 4.0F},
    }));

    world.step();

    const auto position = world.position(player);
    REQUIRE(position.has_value());
    REQUIRE(position->x == Catch::Approx(0.12F));
    REQUIRE(position->y == Catch::Approx(0.16F));
    REQUIRE(world.tick() == mycore::time::Tick{1});
}

TEST_CASE("World applies persistent movement over multiple ticks", "[dots][simulation]") {
    dots::simulation::World world;
    const auto player = world.spawn_player({2.0F, -1.0F});
    REQUIRE(world.apply_input({
        .id = dots::simulation::InputCommandId{4},
        .entity_id = player,
        .movement = {1.0F, 0.0F},
    }));

    for (std::uint32_t tick = 0; tick < dots::simulation::kTickRateHz; ++tick) {
        world.step();
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
    const auto player = world.spawn_player();
    std::uint32_t input_id = 0;

    for (const auto& entry : replay) {
        REQUIRE(world.apply_input({
            .id = dots::simulation::InputCommandId{input_id++},
            .entity_id = player,
            .movement = entry.movement,
        }));
        for (std::size_t tick = 0; tick < entry.tick_count; ++tick) {
            world.step();
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
    const auto player = world.spawn_player();

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

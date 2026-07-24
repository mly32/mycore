#include "dots/simulation/movement.hpp"
#include "dots/simulation/world.hpp"
#include "dots/simulation/world_setup.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

void spawn_food(dots::simulation::World& world, mycore::math::Vector2 position, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        REQUIRE(world.spawn_food(position).has_value());
    }
}

} // namespace

TEST_CASE("World spawns and removes players", "[dots][simulation]") {
    dots::simulation::World world;
    const auto first_result = world.spawn_player(dots::simulation::PlayerOwnerId{0}, {1.0F, 2.0F});
    const auto second_result = world.spawn_player(dots::simulation::PlayerOwnerId{1}, {3.0F, 4.0F});
    REQUIRE(first_result.has_value());
    REQUIRE(second_result.has_value());
    const auto first = *first_result;
    const auto second = *second_result;

    REQUIRE(first != second);
    REQUIRE(world.player_count() == 2);
    REQUIRE(world.occupied_spatial_cell_count() > 0);
    REQUIRE(world.contains(first));
    REQUIRE(world.position(first) == mycore::math::Vector2{1.0F, 2.0F});
    REQUIRE(world.player_owner(first) == dots::simulation::PlayerOwnerId{0});
    REQUIRE(world.player_owner(second) == dots::simulation::PlayerOwnerId{1});

    REQUIRE(world.remove_player(first));
    REQUIRE_FALSE(world.contains(first));
    REQUIRE(world.contains(second));
    REQUIRE(world.position(second) == mycore::math::Vector2{3.0F, 4.0F});
    REQUIRE(world.player_count() == 1);
    REQUIRE_FALSE(world.remove_player(first));
    REQUIRE_FALSE(world.player_owner(first).has_value());
}

TEST_CASE("World rejects players without a valid owner", "[dots][simulation][ownership]") {
    dots::simulation::World world;

    CHECK_FALSE(world.spawn_player(dots::simulation::PlayerOwnerId::invalid()).has_value());
    CHECK(world.player_count() == 0);
    CHECK(world.has_available_entity_id());
}

TEST_CASE("Movement input advances a player by one 30 Hz tick", "[dots][simulation]") {
    dots::simulation::World world;
    const auto player_result = world.spawn_player(dots::simulation::PlayerOwnerId{0});
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

TEST_CASE("Shared player movement normalizes and advances exactly one fixed tick",
          "[dots][simulation][movement]") {
    const auto movement = dots::simulation::normalized_player_movement({3.0F, 4.0F});
    REQUIRE(movement.x == Catch::Approx(0.6F));
    REQUIRE(movement.y == Catch::Approx(0.8F));

    const auto position = dots::simulation::advance_player_position({2.0F, -1.0F}, movement);
    CHECK(position.x == Catch::Approx(2.12F));
    CHECK(position.y == Catch::Approx(-0.84F));
    CHECK(dots::simulation::advance_player_position(
              position, dots::simulation::normalized_player_movement({})) == position);
}

TEST_CASE("World applies persistent movement over multiple ticks", "[dots][simulation]") {
    dots::simulation::World world;
    const auto player_result =
        world.spawn_player(dots::simulation::PlayerOwnerId{0}, {2.0F, -1.0F});
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
    const auto player_result = world.spawn_player(dots::simulation::PlayerOwnerId{0});
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
    const auto player_result = world.spawn_player(dots::simulation::PlayerOwnerId{0});
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
    const auto player_result = world.spawn_player(dots::simulation::PlayerOwnerId{0});
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
    const auto player = world.spawn_player(dots::simulation::PlayerOwnerId{0});
    REQUIRE(player == dots::simulation::EntityId{0});
    REQUIRE_FALSE(world.contains(dots::simulation::EntityId{99}));
    REQUIRE_FALSE(world.position(dots::simulation::EntityId{99}).has_value());
    REQUIRE_FALSE(world.remove_food(dots::simulation::EntityId{99}));
}

TEST_CASE("Broad-phase candidates require exact overlap to be eaten", "[dots][simulation][food]") {
    dots::simulation::World world;
    REQUIRE(world.spawn_player(dots::simulation::PlayerOwnerId{0}).has_value());
    const auto food = world.spawn_food({7.5F, 0.0F});
    REQUIRE(food.has_value());

    REQUIRE(world.step());

    REQUIRE(world.contains(*food));
    REQUIRE(world.food_count() == 1);
}

TEST_CASE("Lowest entity ID wins contested food deterministically", "[dots][simulation][food]") {
    dots::simulation::World world;
    const auto removed = world.spawn_player(dots::simulation::PlayerOwnerId{0});
    const auto lower_id = world.spawn_player(dots::simulation::PlayerOwnerId{0});
    const auto higher_id = world.spawn_player(dots::simulation::PlayerOwnerId{0});
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
    REQUIRE(world.spawn_player(dots::simulation::PlayerOwnerId{0}).has_value());
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
    const auto player_result = world.spawn_player(dots::simulation::PlayerOwnerId{0});
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

TEST_CASE("Equal-mass and same-owner players cannot absorb one another",
          "[dots][simulation][absorption]") {
    dots::simulation::World world;
    const auto first = world.spawn_player(dots::simulation::PlayerOwnerId{0});
    const auto equal = world.spawn_player(dots::simulation::PlayerOwnerId{1});
    const auto same_owner = world.spawn_player(dots::simulation::PlayerOwnerId{0});
    REQUIRE(first.has_value());
    REQUIRE(equal.has_value());
    REQUIRE(same_owner.has_value());

    REQUIRE(world.step());

    CHECK(world.player_count() == 3);
    CHECK(world.last_step_events().empty());
}

TEST_CASE("A strictly larger player absorbs a touching opponent and emits a value event",
          "[dots][simulation][absorption]") {
    dots::simulation::World world;
    const auto absorber = world.spawn_player(dots::simulation::PlayerOwnerId{10});
    REQUIRE(absorber.has_value());
    spawn_food(world, {}, 1);
    REQUIRE(world.step());
    const auto victim = world.spawn_player(dots::simulation::PlayerOwnerId{20});
    REQUIRE(victim.has_value());

    REQUIRE(world.step());

    CHECK(world.player_count() == 1);
    CHECK(world.contains(*absorber));
    CHECK_FALSE(world.contains(*victim));
    CHECK(world.mass(*absorber) ==
          dots::simulation::kInitialPlayerMass * 2.0F + dots::simulation::kFoodMass);
    REQUIRE(world.last_step_events().size() == 1);
    CHECK(world.last_step_events().front() ==
          dots::simulation::PlayerAbsorbed{
              .tick = mycore::time::Tick{2},
              .absorber_entity_id = *absorber,
              .victim_entity_id = *victim,
              .absorber_owner_id = dots::simulation::PlayerOwnerId{10},
              .victim_owner_id = dots::simulation::PlayerOwnerId{20},
              .transferred_mass = dots::simulation::kInitialPlayerMass,
          });

    REQUIRE(world.step());
    CHECK(world.last_step_events().empty());
}

TEST_CASE("Absorption arbitration skips an absorber removed by a larger player",
          "[dots][simulation][absorption][ordering]") {
    dots::simulation::World world;
    const auto largest = world.spawn_player(dots::simulation::PlayerOwnerId{0}, {0.0F, 0.0F});
    const auto middle = world.spawn_player(dots::simulation::PlayerOwnerId{1}, {7.9F, 0.0F});
    const auto smallest = world.spawn_player(dots::simulation::PlayerOwnerId{2}, {15.8F, 0.0F});
    REQUIRE(largest.has_value());
    REQUIRE(middle.has_value());
    REQUIRE(smallest.has_value());
    spawn_food(world, {0.0F, 0.0F}, 3);
    spawn_food(world, {7.9F, 0.0F}, 1);
    REQUIRE(world.step());

    REQUIRE(world.step());

    CHECK(world.contains(*largest));
    CHECK_FALSE(world.contains(*middle));
    CHECK(world.contains(*smallest));
    CHECK(world.mass(*largest) == Catch::Approx(36.0F));
    CHECK(world.mass(*smallest) == Catch::Approx(16.0F));
    REQUIRE(world.last_step_events().size() == 1);
    CHECK(world.last_step_events().front().absorber_entity_id == *largest);
    CHECK(world.last_step_events().front().victim_entity_id == *middle);
}

TEST_CASE("Absorption arbitration orders equal absorbers and multiple victims by entity ID",
          "[dots][simulation][absorption][ordering]") {
    SECTION("Equal absorber masses use the lower absorber entity ID") {
        dots::simulation::World world;
        const auto lower_absorber =
            world.spawn_player(dots::simulation::PlayerOwnerId{0}, {-7.9F, 0.0F});
        const auto higher_absorber =
            world.spawn_player(dots::simulation::PlayerOwnerId{1}, {7.9F, 0.0F});
        const auto victim = world.spawn_player(dots::simulation::PlayerOwnerId{2});
        REQUIRE(lower_absorber.has_value());
        REQUIRE(higher_absorber.has_value());
        REQUIRE(victim.has_value());
        spawn_food(world, {-7.9F, 0.0F}, 1);
        spawn_food(world, {7.9F, 0.0F}, 1);
        REQUIRE(world.step());

        REQUIRE(world.step());

        REQUIRE(world.last_step_events().size() == 1);
        CHECK(world.last_step_events().front().absorber_entity_id == *lower_absorber);
        CHECK(world.last_step_events().front().victim_entity_id == *victim);
        CHECK(world.contains(*higher_absorber));
    }

    SECTION("One absorber emits victims in ascending entity-ID order") {
        dots::simulation::World world;
        const auto absorber = world.spawn_player(dots::simulation::PlayerOwnerId{0});
        REQUIRE(absorber.has_value());
        spawn_food(world, {}, 1);
        REQUIRE(world.step());
        const auto first_victim =
            world.spawn_player(dots::simulation::PlayerOwnerId{1}, {-7.9F, 0.0F});
        const auto second_victim =
            world.spawn_player(dots::simulation::PlayerOwnerId{2}, {7.9F, 0.0F});
        REQUIRE(first_victim.has_value());
        REQUIRE(second_victim.has_value());

        REQUIRE(world.step());

        REQUIRE(world.last_step_events().size() == 2);
        CHECK(world.last_step_events()[0].victim_entity_id == *first_victim);
        CHECK(world.last_step_events()[1].victim_entity_id == *second_victim);
        CHECK(world.mass(*absorber) == Catch::Approx(49.0F));
    }
}

TEST_CASE("Player absorption is resolved before contested food",
          "[dots][simulation][absorption][food]") {
    dots::simulation::World world;
    const auto absorber = world.spawn_player(dots::simulation::PlayerOwnerId{0});
    REQUIRE(absorber.has_value());
    spawn_food(world, {}, 1);
    REQUIRE(world.step());
    const auto victim = world.spawn_player(dots::simulation::PlayerOwnerId{1});
    REQUIRE(victim.has_value());
    const auto contested_food = world.spawn_food({});
    REQUIRE(contested_food.has_value());

    REQUIRE(world.step());

    CHECK_FALSE(world.contains(*victim));
    CHECK_FALSE(world.contains(*contested_food));
    CHECK(world.mass(*absorber) == Catch::Approx(34.0F));
    CHECK(world.last_step_events().size() == 1);
}

TEST_CASE("Absorption growth affects new overlaps on the following tick",
          "[dots][simulation][absorption]") {
    dots::simulation::World world;
    const auto absorber = world.spawn_player(dots::simulation::PlayerOwnerId{0});
    REQUIRE(absorber.has_value());
    spawn_food(world, {}, 1);
    REQUIRE(world.step());
    const auto first_victim = world.spawn_player(dots::simulation::PlayerOwnerId{1});
    const auto later_victim = world.spawn_player(dots::simulation::PlayerOwnerId{2}, {8.5F, 0.0F});
    REQUIRE(first_victim.has_value());
    REQUIRE(later_victim.has_value());

    REQUIRE(world.step());
    CHECK_FALSE(world.contains(*first_victim));
    CHECK(world.contains(*later_victim));

    REQUIRE(world.step());
    CHECK_FALSE(world.contains(*later_victim));
}

TEST_CASE("Indexed spawn candidates follow compact deterministic square rings",
          "[dots][simulation][spawn]") {
    constexpr std::array expected{
        mycore::math::Vector2{0.0F, 0.0F},     mycore::math::Vector2{12.0F, 0.0F},
        mycore::math::Vector2{12.0F, 12.0F},   mycore::math::Vector2{0.0F, 12.0F},
        mycore::math::Vector2{-12.0F, 12.0F},  mycore::math::Vector2{-12.0F, 0.0F},
        mycore::math::Vector2{-12.0F, -12.0F}, mycore::math::Vector2{0.0F, -12.0F},
        mycore::math::Vector2{12.0F, -12.0F},  mycore::math::Vector2{24.0F, -12.0F},
        mycore::math::Vector2{24.0F, 0.0F},    mycore::math::Vector2{24.0F, 12.0F},
        mycore::math::Vector2{24.0F, 24.0F},   mycore::math::Vector2{12.0F, 24.0F},
        mycore::math::Vector2{0.0F, 24.0F},    mycore::math::Vector2{-12.0F, 24.0F},
        mycore::math::Vector2{-24.0F, 24.0F},  mycore::math::Vector2{-24.0F, 12.0F},
        mycore::math::Vector2{-24.0F, 0.0F},   mycore::math::Vector2{-24.0F, -12.0F},
        mycore::math::Vector2{-24.0F, -24.0F}, mycore::math::Vector2{-12.0F, -24.0F},
        mycore::math::Vector2{0.0F, -24.0F},   mycore::math::Vector2{12.0F, -24.0F},
        mycore::math::Vector2{24.0F, -24.0F},
    };

    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK(dots::simulation::initial_player_spawn_candidate(index) == expected[index]);
    }

    constexpr std::array<std::uint64_t, 5> kRings{1, 2, 16, 1'024, 32'768};
    for (const auto ring : kRings) {
        const auto inner_diameter = (ring * 2U) - 1U;
        const auto outer_diameter = (ring * 2U) + 1U;
        const auto ring_start = inner_diameter * inner_diameter;
        const auto ring_end = (outer_diameter * outer_diameter) - 1U;
        const auto world_ring = static_cast<float>(ring * 12U);
        CHECK(dots::simulation::initial_player_spawn_candidate(ring_start) ==
              mycore::math::Vector2{world_ring, -world_ring + 12.0F});
        CHECK(dots::simulation::initial_player_spawn_candidate(ring_end) ==
              mycore::math::Vector2{world_ring, -world_ring});
    }
}

TEST_CASE("Safe player spawning uses active-count indexed rings without overlap",
          "[dots][simulation][spawn]") {
    dots::simulation::World first_world;
    dots::simulation::World second_world;
    constexpr std::uint32_t kPlayerCount = 1'000;

    for (std::uint32_t index = 0; index < kPlayerCount; ++index) {
        const auto owner = dots::simulation::PlayerOwnerId{index};
        const auto first = dots::simulation::spawn_player_safely(first_world, owner);
        const auto second = dots::simulation::spawn_player_safely(second_world, owner);
        const auto* first_id = std::get_if<dots::simulation::EntityId>(&first);
        const auto* second_id = std::get_if<dots::simulation::EntityId>(&second);
        REQUIRE(first_id != nullptr);
        REQUIRE(second_id != nullptr);
        CHECK(first_world.position(*first_id) == second_world.position(*second_id));
        CHECK(first_world.position(*first_id) ==
              dots::simulation::initial_player_spawn_candidate(index));
    }

    CHECK(first_world.player_count() == kPlayerCount);
    CHECK(first_world.position(dots::simulation::EntityId{0}) == mycore::math::Vector2{});
    auto overlap_found = false;
    for (std::size_t first = 0; first < first_world.player_ids().size(); ++first) {
        const auto first_id = first_world.player_ids()[first];
        for (std::size_t second = first + 1; second < first_world.player_ids().size(); ++second) {
            const auto second_id = first_world.player_ids()[second];
            overlap_found = overlap_found || dots::simulation::circles_overlap(
                                                 {.center = *first_world.position(first_id),
                                                  .radius = *first_world.radius(first_id)},
                                                 {.center = *first_world.position(second_id),
                                                  .radius = *first_world.radius(second_id)});
        }
    }
    CHECK_FALSE(overlap_found);
}

TEST_CASE("Safe player spawning classifies exact collisions and ignores food",
          "[dots][simulation][spawn]") {
    dots::simulation::World world;
    CHECK(world.classify_initial_player_spawn({}) ==
          dots::simulation::InitialPlayerSpawnStatus::Clear);
    REQUIRE(world.spawn_food({}).has_value());
    CHECK(world.classify_initial_player_spawn({}) ==
          dots::simulation::InitialPlayerSpawnStatus::Clear);

    const auto first =
        dots::simulation::spawn_player_safely(world, dots::simulation::PlayerOwnerId{0});
    const auto* first_id = std::get_if<dots::simulation::EntityId>(&first);
    REQUIRE(first_id != nullptr);
    CHECK(world.position(*first_id) == mycore::math::Vector2{});
    CHECK(world.classify_initial_player_spawn({8.0F, 0.0F}) ==
          dots::simulation::InitialPlayerSpawnStatus::Blocked);
    CHECK(world.classify_initial_player_spawn({8.1F, 0.0F}) ==
          dots::simulation::InitialPlayerSpawnStatus::Clear);
    CHECK(world.classify_initial_player_spawn({std::numeric_limits<float>::max(), 0.0F}) ==
          dots::simulation::InitialPlayerSpawnStatus::OutsideRepresentableGrid);
}

TEST_CASE("Safe player spawning starts from active count after a removal",
          "[dots][simulation][spawn]") {
    dots::simulation::World world;
    std::array<dots::simulation::EntityId, 3> players;
    for (std::uint32_t index = 0; index < players.size(); ++index) {
        const auto result =
            dots::simulation::spawn_player_safely(world, dots::simulation::PlayerOwnerId{index});
        const auto* player = std::get_if<dots::simulation::EntityId>(&result);
        REQUIRE(player != nullptr);
        players[index] = *player;
    }

    REQUIRE(world.remove_player(players[0]));
    const auto replacement =
        dots::simulation::spawn_player_safely(world, dots::simulation::PlayerOwnerId{3});
    const auto* replacement_id = std::get_if<dots::simulation::EntityId>(&replacement);
    REQUIRE(replacement_id != nullptr);
    CHECK(world.position(*replacement_id) == dots::simulation::initial_player_spawn_candidate(3));
    CHECK(world.classify_initial_player_spawn({}) ==
          dots::simulation::InitialPlayerSpawnStatus::Clear);
}

TEST_CASE("Safe player spawning checks the live radius of grown players",
          "[dots][simulation][spawn]") {
    dots::simulation::World world;
    const auto first =
        dots::simulation::spawn_player_safely(world, dots::simulation::PlayerOwnerId{0});
    const auto* first_id = std::get_if<dots::simulation::EntityId>(&first);
    REQUIRE(first_id != nullptr);
    spawn_food(world, {}, 100);
    REQUIRE(world.step());

    const auto second =
        dots::simulation::spawn_player_safely(world, dots::simulation::PlayerOwnerId{1});
    const auto* second_id = std::get_if<dots::simulation::EntityId>(&second);
    REQUIRE(second_id != nullptr);

    CHECK(world.position(*second_id) == mycore::math::Vector2{12.0F, 12.0F});
    CHECK_FALSE(dots::simulation::circles_overlap(
        {.center = *world.position(*first_id), .radius = *world.radius(*first_id)},
        {.center = *world.position(*second_id), .radius = *world.radius(*second_id)}));
}

#include "dots/simulation/movement.hpp"
#include "dots/simulation/world.hpp"
#include "dots/simulation/world_setup.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace {

void spawn_food(dots::simulation::World& world, mycore::math::Vector2 position, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        REQUIRE(world.spawn_food(position).has_value());
    }
}

[[nodiscard]] dots::simulation::TickCommand input_command(dots::simulation::PlayerOwnerId owner_id,
                                                          dots::simulation::InputCommandId input_id,
                                                          mycore::math::Vector2 movement,
                                                          bool split_requested = false) {
    return {
        .type = dots::simulation::TickCommandType::ApplyInput,
        .input_id = input_id,
        .owner_id = owner_id,
        .movement = movement,
        .split_requested = split_requested,
    };
}

[[nodiscard]] bool advance_with_input(dots::simulation::World& world,
                                      dots::simulation::PlayerOwnerId owner_id,
                                      dots::simulation::InputCommandId input_id,
                                      mycore::math::Vector2 movement) {
    return std::holds_alternative<dots::simulation::TickJournal>(
        world.advance(input_command(owner_id, input_id, movement)));
}

[[nodiscard]] dots::simulation::TickJournal
require_journal(const dots::simulation::TickResult& result) {
    const auto* journal = std::get_if<dots::simulation::TickJournal>(&result);
    REQUIRE(journal != nullptr);
    return *journal;
}

[[nodiscard]] std::vector<dots::simulation::PlayerAbsorbed>
absorption_events(const dots::simulation::World& world) {
    std::vector<dots::simulation::PlayerAbsorbed> result;
    for (const auto& event : world.last_tick_journal().events) {
        if (const auto* absorption = std::get_if<dots::simulation::PlayerAbsorbed>(&event)) {
            result.push_back(*absorption);
        }
    }
    return result;
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

    REQUIRE(advance_with_input(world,
                               dots::simulation::PlayerOwnerId{0},
                               dots::simulation::InputCommandId{0},
                               {3.0F, 4.0F}));

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

TEST_CASE("Held remote movement assumptions preserve authoritative command identity",
          "[dots][simulation][movement][rollback]") {
    constexpr auto owner = dots::simulation::PlayerOwnerId{7};
    dots::simulation::World world;
    REQUIRE(world.spawn_player(owner).has_value());
    REQUIRE(advance_with_input(world, owner, dots::simulation::InputCommandId{5}, {1.0F, 0.0F}));

    const dots::simulation::TickCommand assumption{
        .type = dots::simulation::TickCommandType::AssumeMovement,
        .input_id = dots::simulation::InputCommandId::invalid(),
        .owner_id = owner,
        .movement = {0.0F, 1.0F},
    };
    REQUIRE(std::holds_alternative<dots::simulation::TickJournal>(world.advance(assumption)));
    const auto assumed = world.checkpoint();
    REQUIRE(assumed.owners.size() == 1);
    CHECK(assumed.owners.front().movement == mycore::math::Vector2{0.0F, 1.0F});
    CHECK(assumed.owners.front().last_non_zero_movement == mycore::math::Vector2{0.0F, 1.0F});
    CHECK(assumed.owners.front().last_input_id == dots::simulation::InputCommandId{5});

    REQUIRE(advance_with_input(world, owner, dots::simulation::InputCommandId{6}, {-1.0F, 0.0F}));
    CHECK(world.checkpoint().owners.front().last_input_id == dots::simulation::InputCommandId{6});
}

TEST_CASE("World applies persistent movement over multiple ticks", "[dots][simulation]") {
    dots::simulation::World world;
    const auto player_result =
        world.spawn_player(dots::simulation::PlayerOwnerId{0}, {2.0F, -1.0F});
    REQUIRE(player_result.has_value());
    const auto player = *player_result;
    REQUIRE(advance_with_input(world,
                               dots::simulation::PlayerOwnerId{0},
                               dots::simulation::InputCommandId{4},
                               {1.0F, 0.0F}));
    for (std::uint32_t tick = 1; tick < dots::simulation::kTickRateHz; ++tick) {
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
        REQUIRE(advance_with_input(world,
                                   dots::simulation::PlayerOwnerId{0},
                                   dots::simulation::InputCommandId{input_id++},
                                   entry.movement));
        for (std::size_t tick = 1; tick < entry.tick_count; ++tick) {
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
    REQUIRE(std::get<dots::simulation::TickError>(world.advance(
                input_command(dots::simulation::PlayerOwnerId{0},
                              dots::simulation::InputCommandId::invalid(),
                              {1.0F, 0.0F}))) == dots::simulation::TickError::InvalidCommand);
    REQUIRE(std::get<dots::simulation::TickError>(world.advance(
                input_command(dots::simulation::PlayerOwnerId{99},
                              dots::simulation::InputCommandId{0},
                              {1.0F, 0.0F}))) == dots::simulation::TickError::InvalidCommand);
    REQUIRE(advance_with_input(world,
                               dots::simulation::PlayerOwnerId{0},
                               dots::simulation::InputCommandId{2},
                               {1.0F, 0.0F}));
    REQUIRE(std::get<dots::simulation::TickError>(world.advance(
                input_command(dots::simulation::PlayerOwnerId{0},
                              dots::simulation::InputCommandId{1},
                              {0.0F, 1.0F}))) == dots::simulation::TickError::InvalidCommand);
}

TEST_CASE("World checkpoints round trip complete replay state and allocator",
          "[dots][simulation][checkpoint]") {
    auto rules = dots::simulation::WorldRules{};
    rules.initial_player_mass = 25.0F;
    rules.food_mass = 2.0F;
    rules.spatial_grid_cell_size = 4.0F;
    rules.player_speed_units_per_second = 9.0F;
    dots::simulation::World world{rules};
    constexpr auto owner = dots::simulation::PlayerOwnerId{7};
    constexpr auto prediction_key = dots::simulation::PredictionKey{
        .owner_id = owner,
        .input_id = dots::simulation::InputCommandId{4},
        .child_ordinal = 2,
    };
    const auto predicted_player = world.spawn_player(owner, {-20.0F, 2.0F}, prediction_key);
    const auto sibling = world.spawn_player(owner, {20.0F, 2.0F});
    const auto food = world.spawn_food({50.0F, -4.0F});
    REQUIRE(predicted_player.has_value());
    REQUIRE(sibling.has_value());
    REQUIRE(food.has_value());

    const auto journal = require_journal(
        world.advance(input_command(owner, dots::simulation::InputCommandId{5}, {3.0F, 4.0F})));
    REQUIRE(journal.tick == mycore::time::Tick{1});
    REQUIRE(journal.events.empty());
    const auto checkpoint = world.checkpoint();

    REQUIRE(checkpoint.rules == rules);
    REQUIRE(checkpoint.owners.size() == 1);
    CHECK(checkpoint.owners.front().owner_id == owner);
    CHECK(checkpoint.owners.front().player_ids == std::vector{*predicted_player, *sibling});
    CHECK(checkpoint.owners.front().movement.x == Catch::Approx(0.6F));
    CHECK(checkpoint.owners.front().movement.y == Catch::Approx(0.8F));
    CHECK(checkpoint.owners.front().last_non_zero_movement == checkpoint.owners.front().movement);
    CHECK(checkpoint.owners.front().last_input_id == dots::simulation::InputCommandId{5});
    REQUIRE(checkpoint.players.size() == 2);
    CHECK(checkpoint.players.front().entity_id == *predicted_player);
    CHECK(checkpoint.players.front().prediction_key == prediction_key);
    CHECK(world.mass(*predicted_player) == 25.0F);
    CHECK(world.radius(*predicted_player) == 5.0F);
    CHECK(world.position(*predicted_player) == mycore::math::Vector2{-19.82F, 2.24F});

    dots::simulation::World restored;
    REQUIRE_FALSE(restored.restore(checkpoint).has_value());
    CHECK(restored.checkpoint() == checkpoint);
    CHECK(restored.rules() == rules);
    CHECK(restored.prediction_key(*predicted_player) == prediction_key);
    CHECK(restored.radius(*predicted_player) == 5.0F);
    CHECK(restored.occupied_spatial_cell_count() > 0);
    CHECK(restored.last_tick_journal() ==
          dots::simulation::TickJournal{.tick = checkpoint.tick, .events = {}});

    const auto original_next_entity = world.spawn_food({80.0F, 0.0F});
    const auto restored_next_entity = restored.spawn_food({80.0F, 0.0F});
    REQUIRE(original_next_entity.has_value());
    REQUIRE(restored_next_entity.has_value());
    CHECK(original_next_entity == restored_next_entity);
}

TEST_CASE("Owner commands are atomic, order independent, and apply to every owned piece",
          "[dots][simulation][tick]") {
    dots::simulation::World original;
    constexpr auto first_owner = dots::simulation::PlayerOwnerId{1};
    constexpr auto second_owner = dots::simulation::PlayerOwnerId{2};
    const auto first_piece = original.spawn_player(first_owner, {-20.0F, 0.0F});
    const auto second_piece = original.spawn_player(first_owner, {-10.0F, 0.0F});
    const auto remote_piece = original.spawn_player(second_owner, {20.0F, 0.0F});
    REQUIRE(first_piece.has_value());
    REQUIRE(second_piece.has_value());
    REQUIRE(remote_piece.has_value());

    dots::simulation::World reversed;
    REQUIRE_FALSE(reversed.restore(original.checkpoint()).has_value());
    const std::array commands{
        input_command(second_owner, dots::simulation::InputCommandId{0}, {0.0F, 1.0F}),
        input_command(first_owner, dots::simulation::InputCommandId{0}, {1.0F, 0.0F}),
    };
    const std::array reversed_commands{commands[1], commands[0]};

    CHECK(require_journal(original.advance(commands)) ==
          require_journal(reversed.advance(reversed_commands)));
    CHECK(original.checkpoint() == reversed.checkpoint());
    CHECK(original.position(*first_piece) == mycore::math::Vector2{-19.8F, 0.0F});
    CHECK(original.position(*second_piece) == mycore::math::Vector2{-9.8F, 0.0F});
    CHECK(original.position(*remote_piece) == mycore::math::Vector2{20.0F, 0.2F});

    const auto checkpoint_before_rejection = original.checkpoint();
    const auto journal_before_rejection = original.last_tick_journal();
    const std::array duplicate_owner_commands{
        input_command(first_owner, dots::simulation::InputCommandId{1}, {0.0F, 1.0F}),
        input_command(first_owner, dots::simulation::InputCommandId{2}, {-1.0F, 0.0F}),
    };
    const auto rejection = original.advance(duplicate_owner_commands);
    REQUIRE(std::get<dots::simulation::TickError>(rejection) ==
            dots::simulation::TickError::DuplicateOwnerCommand);
    CHECK(original.checkpoint() == checkpoint_before_rejection);
    CHECK(original.last_tick_journal() == journal_before_rejection);
}

TEST_CASE("Checkpoint restore rejects invalid state without changing the World",
          "[dots][simulation][checkpoint][atomic]") {
    dots::simulation::World world;
    constexpr auto owner = dots::simulation::PlayerOwnerId{3};
    REQUIRE(world.spawn_player(owner).has_value());
    REQUIRE(world.spawn_player(dots::simulation::PlayerOwnerId{4}, {40.0F, 0.0F}).has_value());
    REQUIRE(world.spawn_food({30.0F, 0.0F}).has_value());
    REQUIRE(advance_with_input(world, owner, dots::simulation::InputCommandId{0}, {1.0F, 0.0F}));
    const auto checkpoint_before_rejection = world.checkpoint();
    const auto journal_before_rejection = world.last_tick_journal();

    auto invalid_rules = checkpoint_before_rejection;
    invalid_rules.rules.player_speed_units_per_second = 0.0F;
    CHECK(world.restore(invalid_rules) == dots::simulation::CheckpointRestoreError::InvalidRules);
    CHECK(world.checkpoint() == checkpoint_before_rejection);
    CHECK(world.last_tick_journal() == journal_before_rejection);

    auto invalid_ordering = checkpoint_before_rejection;
    std::swap(invalid_ordering.players[0], invalid_ordering.players[1]);
    CHECK(world.restore(invalid_ordering) ==
          dots::simulation::CheckpointRestoreError::InvalidOrdering);
    CHECK(world.checkpoint() == checkpoint_before_rejection);
    CHECK(world.last_tick_journal() == journal_before_rejection);

    auto invalid_membership = checkpoint_before_rejection;
    invalid_membership.owners.front().player_ids.clear();
    CHECK(world.restore(invalid_membership) ==
          dots::simulation::CheckpointRestoreError::InvalidOwnerState);
    CHECK(world.checkpoint() == checkpoint_before_rejection);
    CHECK(world.last_tick_journal() == journal_before_rejection);

    auto invalid_entity = checkpoint_before_rejection;
    invalid_entity.players.front().mass = 0.0F;
    CHECK(world.restore(invalid_entity) ==
          dots::simulation::CheckpointRestoreError::InvalidEntityState);
    CHECK(world.checkpoint() == checkpoint_before_rejection);
    CHECK(world.last_tick_journal() == journal_before_rejection);

    auto duplicate_prediction_key = checkpoint_before_rejection;
    const auto prediction_key = dots::simulation::PredictionKey{
        .owner_id = duplicate_prediction_key.players.front().owner_id,
        .input_id = dots::simulation::InputCommandId{20},
        .child_ordinal = 0,
    };
    duplicate_prediction_key.players.front().prediction_key = prediction_key;
    duplicate_prediction_key.players[1].owner_id =
        duplicate_prediction_key.players.front().owner_id;
    duplicate_prediction_key.players[1].prediction_key = prediction_key;
    duplicate_prediction_key.owners.front().player_ids.push_back(
        duplicate_prediction_key.players[1].entity_id);
    duplicate_prediction_key.owners.erase(duplicate_prediction_key.owners.begin() + 1);
    CHECK(world.restore(duplicate_prediction_key) ==
          dots::simulation::CheckpointRestoreError::InvalidEntityState);
    CHECK(world.checkpoint() == checkpoint_before_rejection);

    auto invalid_geometry = checkpoint_before_rejection;
    invalid_geometry.players.front().position.x = std::numeric_limits<float>::max();
    CHECK(world.restore(invalid_geometry) ==
          dots::simulation::CheckpointRestoreError::InvalidGeometry);
    CHECK(world.checkpoint() == checkpoint_before_rejection);
    CHECK(world.last_tick_journal() == journal_before_rejection);
}

TEST_CASE("Failed simulation advance leaves command and World state uncommitted",
          "[dots][simulation][tick][atomic]") {
    auto rules = dots::simulation::WorldRules{};
    rules.player_speed_units_per_second = std::numeric_limits<float>::max();
    dots::simulation::World world{rules};
    constexpr auto owner = dots::simulation::PlayerOwnerId{0};
    const auto player = world.spawn_player(owner);
    REQUIRE(player.has_value());
    const auto checkpoint_before_rejection = world.checkpoint();
    const auto journal_before_rejection = world.last_tick_journal();

    const auto rejected =
        world.advance(input_command(owner, dots::simulation::InputCommandId{0}, {1.0F, 0.0F}));
    REQUIRE(std::get<dots::simulation::TickError>(rejected) ==
            dots::simulation::TickError::SimulationRejected);
    CHECK(world.checkpoint() == checkpoint_before_rejection);
    CHECK(world.last_tick_journal() == journal_before_rejection);

    REQUIRE(world.step());
    CHECK(world.tick() == mycore::time::Tick{1});
    CHECK(world.position(*player) == mycore::math::Vector2{});

    auto exhausted_tick = world.checkpoint();
    exhausted_tick.tick = mycore::time::Tick{std::numeric_limits<std::uint64_t>::max()};
    REQUIRE_FALSE(world.restore(exhausted_tick).has_value());
    const auto checkpoint_at_exhaustion = world.checkpoint();
    const auto exhausted = world.advance(std::span<const dots::simulation::TickCommand>{});
    REQUIRE(std::get<dots::simulation::TickError>(exhausted) ==
            dots::simulation::TickError::SimulationRejected);
    CHECK(world.checkpoint() == checkpoint_at_exhaustion);
}

TEST_CASE("Checkpoint replay regenerates deterministic typed event journals",
          "[dots][simulation][checkpoint][replay][events]") {
    dots::simulation::World source;
    constexpr auto first_owner = dots::simulation::PlayerOwnerId{10};
    constexpr auto second_owner = dots::simulation::PlayerOwnerId{20};
    const auto first = source.spawn_player(first_owner);
    const auto second = source.spawn_player(second_owner);
    const auto food = source.spawn_food({});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(food.has_value());

    dots::simulation::World first_replay;
    dots::simulation::World second_replay;
    const auto starting_checkpoint = source.checkpoint();
    REQUIRE_FALSE(first_replay.restore(starting_checkpoint).has_value());
    REQUIRE_FALSE(second_replay.restore(starting_checkpoint).has_value());

    const std::array first_tick_commands{
        input_command(second_owner, dots::simulation::InputCommandId{0}, {1.0F, 0.0F}),
        input_command(first_owner, dots::simulation::InputCommandId{0}, {1.0F, 0.0F}),
    };
    const auto first_journal = require_journal(first_replay.advance(first_tick_commands));
    const auto replayed_first_journal = require_journal(second_replay.advance(first_tick_commands));
    REQUIRE(first_journal == replayed_first_journal);
    REQUIRE(first_journal.events.size() == 1);
    const auto* consumed = std::get_if<dots::simulation::FoodConsumed>(&first_journal.events[0]);
    REQUIRE(consumed != nullptr);
    CHECK(*consumed == dots::simulation::FoodConsumed{
                           .tick = mycore::time::Tick{1},
                           .food_entity_id = *food,
                           .consumer_entity_id = *first,
                           .consumer_owner_id = first_owner,
                           .food_position = {},
                           .transferred_mass = dots::simulation::kFoodMass,
                       });
    CHECK(dots::simulation::simulation_event_key(first_journal.events[0]) ==
          dots::simulation::SimulationEventKey{
              dots::simulation::FoodConsumedKey{.food_entity_id = *food}});
    CHECK(first_replay.checkpoint() == second_replay.checkpoint());

    const std::array second_tick_commands{
        dots::simulation::TickCommand{
            .type = dots::simulation::TickCommandType::StopMovement,
            .input_id = dots::simulation::InputCommandId::invalid(),
            .owner_id = first_owner,
            .movement = {},
        },
        dots::simulation::TickCommand{
            .type = dots::simulation::TickCommandType::StopMovement,
            .input_id = dots::simulation::InputCommandId::invalid(),
            .owner_id = second_owner,
            .movement = {},
        },
    };
    const auto second_journal = require_journal(first_replay.advance(second_tick_commands));
    const auto replayed_second_journal =
        require_journal(second_replay.advance(second_tick_commands));
    REQUIRE(second_journal == replayed_second_journal);
    REQUIRE(second_journal.events.size() == 1);
    const auto* absorbed = std::get_if<dots::simulation::PlayerAbsorbed>(&second_journal.events[0]);
    REQUIRE(absorbed != nullptr);
    CHECK(*absorbed == dots::simulation::PlayerAbsorbed{
                           .tick = mycore::time::Tick{2},
                           .absorber_entity_id = *first,
                           .victim_entity_id = *second,
                           .absorber_owner_id = first_owner,
                           .victim_owner_id = second_owner,
                           .absorber_position = {0.2F, 0.0F},
                           .victim_position = {0.2F, 0.0F},
                           .transferred_mass = dots::simulation::kInitialPlayerMass,
                       });
    CHECK(dots::simulation::simulation_event_key(second_journal.events[0]) ==
          dots::simulation::SimulationEventKey{
              dots::simulation::PlayerAbsorbedKey{.victim_entity_id = *second}});
    CHECK(first_replay.checkpoint() == second_replay.checkpoint());
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

    REQUIRE(advance_with_input(world,
                               dots::simulation::PlayerOwnerId{0},
                               dots::simulation::InputCommandId{0},
                               {1.0F, 0.0F}));
    for (std::size_t tick = 1; tick < 3; ++tick) {
        REQUIRE(world.step());
    }
    REQUIRE(world.food_count() == 1);

    REQUIRE(advance_with_input(world,
                               dots::simulation::PlayerOwnerId{0},
                               dots::simulation::InputCommandId{1},
                               {0.0F, 1.0F}));
    for (std::size_t tick = 1; tick < 3; ++tick) {
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
    CHECK(absorption_events(world).empty());
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
    const auto absorbed = absorption_events(world);
    REQUIRE(absorbed.size() == 1);
    CHECK(absorbed.front() == dots::simulation::PlayerAbsorbed{
                                  .tick = mycore::time::Tick{2},
                                  .absorber_entity_id = *absorber,
                                  .victim_entity_id = *victim,
                                  .absorber_owner_id = dots::simulation::PlayerOwnerId{10},
                                  .victim_owner_id = dots::simulation::PlayerOwnerId{20},
                                  .absorber_position = {},
                                  .victim_position = {},
                                  .transferred_mass = dots::simulation::kInitialPlayerMass,
                              });

    REQUIRE(world.step());
    CHECK(absorption_events(world).empty());
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
    const auto absorbed = absorption_events(world);
    REQUIRE(absorbed.size() == 1);
    CHECK(absorbed.front().absorber_entity_id == *largest);
    CHECK(absorbed.front().victim_entity_id == *middle);
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

        const auto absorbed = absorption_events(world);
        REQUIRE(absorbed.size() == 1);
        CHECK(absorbed.front().absorber_entity_id == *lower_absorber);
        CHECK(absorbed.front().victim_entity_id == *victim);
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

        const auto absorbed = absorption_events(world);
        REQUIRE(absorbed.size() == 2);
        CHECK(absorbed[0].victim_entity_id == *first_victim);
        CHECK(absorbed[1].victim_entity_id == *second_victim);
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
    CHECK(absorption_events(world).size() == 1);
    REQUIRE(world.last_tick_journal().events.size() == 2);
    CHECK(std::holds_alternative<dots::simulation::PlayerAbsorbed>(
        world.last_tick_journal().events[0]));
    CHECK(std::holds_alternative<dots::simulation::FoodConsumed>(
        world.last_tick_journal().events[1]));
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

TEST_CASE("Split creates stable predicted children with launch and owner cooldown",
          "[dots][simulation][split]") {
    auto rules = dots::simulation::WorldRules{};
    rules.initial_player_mass = 64.0F;
    dots::simulation::World world{rules};
    constexpr auto owner = dots::simulation::PlayerOwnerId{7};
    const auto parent = world.spawn_player(owner);
    REQUIRE(parent.has_value());

    const auto journal = require_journal(world.advance(
        input_command(owner, dots::simulation::InputCommandId{3}, {0.0F, 1.0F}, true)));
    REQUIRE(journal.events.size() == 1);
    const auto* split = std::get_if<dots::simulation::PlayerSplit>(&journal.events.front());
    REQUIRE(split != nullptr);
    CHECK(split->tick == mycore::time::Tick{1});
    CHECK(split->parent_entity_id == *parent);
    CHECK(split->child_entity_id == dots::simulation::EntityId{1});
    CHECK(split->parent_mass == 32.0F);
    CHECK(split->child_mass == 32.0F);
    CHECK(dots::simulation::simulation_event_key(journal.events.front()) ==
          dots::simulation::SimulationEventKey{dots::simulation::PlayerSplitKey{
              .owner_id = owner,
              .input_id = dots::simulation::InputCommandId{3},
              .child_ordinal = 0,
          }});

    const auto checkpoint = world.checkpoint();
    REQUIRE(checkpoint.owners.size() == 1);
    REQUIRE(checkpoint.players.size() == 2);
    CHECK(checkpoint.owners.front().split_cooldown_end_tick == mycore::time::Tick{16});
    CHECK(checkpoint.players[0].mass == 32.0F);
    CHECK(checkpoint.players[1].mass == 32.0F);
    CHECK(checkpoint.players[0].merge_eligible_tick == mycore::time::Tick{151});
    CHECK(checkpoint.players[1].merge_eligible_tick == mycore::time::Tick{151});
    CHECK(checkpoint.players[1].prediction_key ==
          dots::simulation::PredictionKey{
              .owner_id = owner,
              .input_id = dots::simulation::InputCommandId{3},
              .child_ordinal = 0,
          });
    CHECK(checkpoint.players[0].position.x == Catch::Approx(0.0F));
    CHECK(checkpoint.players[0].position.y == Catch::Approx(0.2F));
    CHECK(checkpoint.players[1].position.x == Catch::Approx(0.0F));
    CHECK(checkpoint.players[1].position.y == Catch::Approx(0.8F));
    CHECK(checkpoint.players[1].launch_velocity.y == Catch::Approx(17.4F));

    const auto cooldown_journal = require_journal(world.advance(
        input_command(owner, dots::simulation::InputCommandId{4}, {0.0F, 1.0F}, true)));
    CHECK(cooldown_journal.events.empty());
    CHECK(world.player_count() == 2);

    for (auto tick = 0; tick < 13; ++tick) {
        REQUIRE(world.step());
    }
    const auto recast_journal = require_journal(world.advance(
        input_command(owner, dots::simulation::InputCommandId{5}, {0.0F, 1.0F}, true)));
    CHECK(world.tick() == mycore::time::Tick{16});
    CHECK(recast_journal.events.size() == 2);
    CHECK(world.player_count() == 4);
}

TEST_CASE("Existing launch integrates and decays when split topology is disabled",
          "[dots][simulation][movement][split]") {
    auto rules = dots::simulation::WorldRules{};
    rules.launch_decay_units_per_second_squared = 6.0F;
    dots::simulation::World world{rules};
    const auto player = world.spawn_player(dots::simulation::PlayerOwnerId{0});
    REQUIRE(player.has_value());

    auto checkpoint = world.checkpoint();
    checkpoint.players.front().launch_velocity = {12.0F, 0.0F};
    REQUIRE_FALSE(world.restore(checkpoint).has_value());

    const auto journal =
        require_journal(world.advance(std::span<const dots::simulation::TickCommand>{},
                                      {
                                          .player_absorption = false,
                                          .food_consumption = false,
                                          .split_merge = false,
                                      }));
    CHECK(journal.events.empty());
    const auto advanced = world.checkpoint();
    CHECK(advanced.players.front().position.x == Catch::Approx(0.4F));
    CHECK(advanced.players.front().launch_velocity.x == Catch::Approx(11.8F));
}

TEST_CASE("Simulation events expose canonical unique owner participants",
          "[dots][simulation][events]") {
    const dots::simulation::SimulationEvent absorbed = dots::simulation::PlayerAbsorbed{
        .tick = mycore::time::Tick{2},
        .absorber_entity_id = dots::simulation::EntityId{10},
        .victim_entity_id = dots::simulation::EntityId{20},
        .absorber_owner_id = dots::simulation::PlayerOwnerId{7},
        .victim_owner_id = dots::simulation::PlayerOwnerId{3},
        .transferred_mass = 16.0F,
    };
    const auto participants = dots::simulation::simulation_event_participants(absorbed);
    REQUIRE(participants.count == 2);
    CHECK(participants.owner_ids[0] == dots::simulation::PlayerOwnerId{3});
    CHECK(participants.owner_ids[1] == dots::simulation::PlayerOwnerId{7});
    CHECK(dots::simulation::simulation_event_involves_owner(absorbed,
                                                            dots::simulation::PlayerOwnerId{3}));
    CHECK_FALSE(dots::simulation::simulation_event_involves_owner(
        absorbed, dots::simulation::PlayerOwnerId{9}));
}

TEST_CASE("Split processes stable parent order and respects the owner piece cap",
          "[dots][simulation][split]") {
    auto rules = dots::simulation::WorldRules{};
    rules.initial_player_mass = 32.0F;
    rules.maximum_pieces_per_owner = 3;
    dots::simulation::World world{rules};
    constexpr auto owner = dots::simulation::PlayerOwnerId{2};
    const auto first = world.spawn_player(owner, {-20.0F, 0.0F});
    const auto second = world.spawn_player(owner, {20.0F, 0.0F});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    const auto journal = require_journal(
        world.advance(input_command(owner, dots::simulation::InputCommandId{0}, {}, true)));
    REQUIRE(journal.events.size() == 1);
    const auto* split = std::get_if<dots::simulation::PlayerSplit>(&journal.events.front());
    REQUIRE(split != nullptr);
    CHECK(split->parent_entity_id == *first);
    CHECK(split->child_entity_id == dots::simulation::EntityId{2});
    CHECK(world.player_count() == 3);
    CHECK(world.mass(*first) == 16.0F);
    CHECK(world.mass(*second) == 32.0F);
    CHECK(world.mass(dots::simulation::EntityId{2}) == 16.0F);
    CHECK(world.position(dots::simulation::EntityId{2})->x == Catch::Approx(-19.4F));
    CHECK(world.checkpoint().players[2].launch_velocity.x == Catch::Approx(17.4F));
}

TEST_CASE("Split pieces merge only after their deadline and conserve mass",
          "[dots][simulation][merge]") {
    auto rules = dots::simulation::WorldRules{};
    rules.child_launch_speed_units_per_second = 0.0F;
    rules.cohesion_speed_units_per_second = 0.0F;
    rules.merge_delay_ticks = 2;
    dots::simulation::World world{rules};
    constexpr auto owner = dots::simulation::PlayerOwnerId{5};
    const auto parent = world.spawn_player(owner);
    REQUIRE(parent.has_value());

    const auto split_journal = require_journal(
        world.advance(input_command(owner, dots::simulation::InputCommandId{0}, {}, true)));
    REQUIRE(std::holds_alternative<dots::simulation::PlayerSplit>(split_journal.events.front()));
    REQUIRE(world.player_count() == 2);
    REQUIRE(world.step());
    CHECK(world.player_count() == 2);

    const auto merge_journal =
        require_journal(world.advance(std::span<const dots::simulation::TickCommand>{}));
    REQUIRE(merge_journal.events.size() == 1);
    const auto* merged = std::get_if<dots::simulation::PiecesMerged>(&merge_journal.events.front());
    REQUIRE(merged != nullptr);
    CHECK(merged->tick == mycore::time::Tick{3});
    CHECK(merged->survivor_entity_id == *parent);
    CHECK(merged->consumed_entity_id == dots::simulation::EntityId{1});
    CHECK(merged->combined_mass == dots::simulation::kInitialPlayerMass);
    CHECK(dots::simulation::simulation_event_key(merge_journal.events.front()) ==
          dots::simulation::SimulationEventKey{dots::simulation::PiecesMergedKey{
              .first_entity_id = *parent,
              .second_entity_id = dots::simulation::EntityId{1},
          }});
    CHECK(world.player_count() == 1);
    CHECK(world.mass(*parent) == dots::simulation::kInitialPlayerMass);
}

TEST_CASE("Merge-eligible pieces cohere toward their mass-weighted owner centroid",
          "[dots][simulation][merge][cohesion]") {
    dots::simulation::World source;
    constexpr auto owner = dots::simulation::PlayerOwnerId{9};
    const auto first = source.spawn_player(owner, {-10.0F, 0.0F});
    const auto second = source.spawn_player(owner, {10.0F, 0.0F});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    auto checkpoint = source.checkpoint();
    checkpoint.players[0].merge_eligible_tick = {};
    checkpoint.players[1].merge_eligible_tick = {};

    dots::simulation::World world;
    REQUIRE_FALSE(world.restore(checkpoint).has_value());
    const auto journal =
        require_journal(world.advance(std::span<const dots::simulation::TickCommand>{}));
    CHECK(journal.events.empty());
    CHECK(world.position(*first)->x == Catch::Approx(-9.9F));
    CHECK(world.position(*second)->x == Catch::Approx(9.9F));
}

TEST_CASE("Stable merge uses mass-weighted position and launch velocity",
          "[dots][simulation][merge]") {
    auto rules = dots::simulation::WorldRules{};
    rules.launch_decay_units_per_second_squared = 0.0F;
    rules.cohesion_speed_units_per_second = 0.0F;
    dots::simulation::World source{rules};
    constexpr auto owner = dots::simulation::PlayerOwnerId{13};
    const auto first = source.spawn_player(owner, {-1.0F, 0.0F});
    const auto second = source.spawn_player(owner, {2.0F, 0.0F});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    auto checkpoint = source.checkpoint();
    checkpoint.players[0].mass = 9.0F;
    checkpoint.players[0].launch_velocity = {3.0F, 0.0F};
    checkpoint.players[0].merge_eligible_tick = {};
    checkpoint.players[1].mass = 16.0F;
    checkpoint.players[1].launch_velocity = {-1.0F, 0.0F};
    checkpoint.players[1].merge_eligible_tick = {};

    dots::simulation::World world{rules};
    REQUIRE_FALSE(world.restore(checkpoint).has_value());
    const auto journal =
        require_journal(world.advance(std::span<const dots::simulation::TickCommand>{}));
    REQUIRE(journal.events.size() == 1);
    REQUIRE(std::holds_alternative<dots::simulation::PiecesMerged>(journal.events.front()));
    CHECK(world.player_count() == 1);
    CHECK(world.contains(*first));
    CHECK_FALSE(world.contains(*second));
    CHECK(world.mass(*first) == 25.0F);
    CHECK(world.position(*first)->x == Catch::Approx(0.9346667F));
    CHECK(world.checkpoint().players.front().launch_velocity.x == Catch::Approx(0.44F));
}

TEST_CASE("Checkpoint replay regenerates identical split and merge structure",
          "[dots][simulation][checkpoint][replay][split][merge]") {
    auto rules = dots::simulation::WorldRules{};
    rules.child_launch_speed_units_per_second = 0.0F;
    rules.cohesion_speed_units_per_second = 0.0F;
    rules.merge_delay_ticks = 2;
    dots::simulation::World source{rules};
    constexpr auto owner = dots::simulation::PlayerOwnerId{12};
    REQUIRE(source.spawn_player(owner).has_value());

    dots::simulation::World first{rules};
    dots::simulation::World second{rules};
    REQUIRE_FALSE(first.restore(source.checkpoint()).has_value());
    REQUIRE_FALSE(second.restore(source.checkpoint()).has_value());
    const auto split = input_command(owner, dots::simulation::InputCommandId{0}, {}, true);
    CHECK(require_journal(first.advance(split)) == require_journal(second.advance(split)));
    CHECK(first.checkpoint() == second.checkpoint());
    CHECK(require_journal(first.advance(std::span<const dots::simulation::TickCommand>{})) ==
          require_journal(second.advance(std::span<const dots::simulation::TickCommand>{})));
    const auto first_merge =
        require_journal(first.advance(std::span<const dots::simulation::TickCommand>{}));
    const auto second_merge =
        require_journal(second.advance(std::span<const dots::simulation::TickCommand>{}));
    CHECK(first_merge == second_merge);
    REQUIRE(first_merge.events.size() == 1);
    CHECK(std::holds_alternative<dots::simulation::PiecesMerged>(first_merge.events.front()));
    CHECK(first.checkpoint() == second.checkpoint());
}

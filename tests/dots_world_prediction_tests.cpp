#include "dots/prediction/prediction.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace {

using dots::prediction::AuthorityFrame;
using dots::prediction::Commit;
using dots::prediction::PredictionMechanic;
using dots::prediction::PredictionProfile;
using dots::prediction::PredictionRequest;
using dots::prediction::PredictionScope;
using dots::prediction::TickStimulus;
using dots::simulation::EntityId;
using dots::simulation::InputCommandId;
using dots::simulation::PlayerOwnerId;
using dots::simulation::TickCommand;
using dots::simulation::TickCommandType;
using dots::simulation::World;
using dots::simulation::WorldCheckpoint;
using mycore::math::Vector2;

[[nodiscard]] TickCommand
input(PlayerOwnerId owner_id, std::uint32_t input_id, Vector2 movement = {}) {
    return {
        .type = TickCommandType::ApplyInput,
        .input_id = InputCommandId{input_id},
        .owner_id = owner_id,
        .movement = movement,
    };
}

[[nodiscard]] dots::prediction::HeldMovementAssumption
remote(PlayerOwnerId owner_id, mycore::time::Tick source_tick, Vector2 movement = {}) {
    return {.owner_id = owner_id, .source_tick = source_tick, .movement = movement};
}

[[nodiscard]] PredictionScope
require_scope(const WorldCheckpoint& checkpoint,
              PredictionProfile profile,
              dots::prediction::MechanicMask mechanics,
              std::vector<PlayerOwnerId> owned_owner_ids,
              mycore::time::TickDelta horizon = mycore::time::TickDelta{1},
              dots::prediction::AuthorityCoverage coverage = {}) {
    const auto result =
        dots::prediction::build_prediction_scope(checkpoint,
                                                 {
                                                     .profile = profile,
                                                     .mechanics = mechanics,
                                                     .owned_owner_ids = std::move(owned_owner_ids),
                                                     .replay_horizon = horizon,
                                                     .scope_epoch = mycore::rollback::ScopeEpoch{1},
                                                     .coverage = coverage,
                                                 });
    const auto* scope = std::get_if<PredictionScope>(&result);
    REQUIRE(scope != nullptr);
    return *scope;
}

[[nodiscard]] WorldCheckpoint require_projection(const WorldCheckpoint& checkpoint,
                                                 const PredictionScope& scope) {
    const auto result = dots::prediction::project_checkpoint(checkpoint, scope);
    const auto* projection = std::get_if<WorldCheckpoint>(&result);
    REQUIRE(projection != nullptr);
    return *projection;
}

[[nodiscard]] Commit require_commit(const dots::prediction::CommitResult& result) {
    const auto* commit = std::get_if<Commit>(&result);
    REQUIRE(commit != nullptr);
    return *commit;
}

[[nodiscard]] dots::prediction::Timeline initialized_timeline(const WorldCheckpoint& checkpoint,
                                                              const PredictionScope& scope) {
    dots::prediction::Timeline timeline{dots::prediction::WorldModel{}};
    const auto initialized = timeline.initialize(
        {
            .tick = checkpoint.tick,
            .acknowledged_through = std::nullopt,
            .scope_epoch = scope.scope_epoch,
            .checkpoint = checkpoint,
            .events = {},
        },
        scope);
    static_cast<void>(require_commit(initialized));
    return timeline;
}

[[nodiscard]] AuthorityFrame authority_frame(const WorldCheckpoint& checkpoint,
                                             const PredictionScope& scope,
                                             std::optional<std::uint64_t> acknowledged) {
    return {
        .tick = checkpoint.tick,
        .acknowledged_through =
            acknowledged ? std::optional<
                               mycore::rollback::CommandSequence>{mycore::rollback::CommandSequence{
                               *acknowledged}}
                         : std::nullopt,
        .scope_epoch = scope.scope_epoch,
        .checkpoint = checkpoint,
        .events = {},
    };
}

} // namespace

TEST_CASE("Dots mechanic contracts declare deterministic prediction dependencies",
          "[dots][prediction][scope]") {
    const auto& movement = dots::prediction::mechanic_contract(PredictionMechanic::Movement);
    const auto& food = dots::prediction::mechanic_contract(PredictionMechanic::FoodConsumption);
    const auto& absorption =
        dots::prediction::mechanic_contract(PredictionMechanic::PlayerAbsorption);
    const auto& split_merge = dots::prediction::mechanic_contract(PredictionMechanic::SplitMerge);

    CHECK(movement.implemented);
    CHECK(dots::prediction::includes_state_domain(movement.writes,
                                                  dots::prediction::StateDomain::PlayerKinematics));
    CHECK(dots::prediction::includes_mechanic(food.dependencies, PredictionMechanic::Movement));
    CHECK(food.expands_spatial_interactions);
    CHECK(dots::prediction::includes_mechanic_event(
        food.events, dots::prediction::MechanicEventKind::FoodConsumed));
    CHECK(dots::prediction::includes_state_domain(movement.presentation_reads,
                                                  dots::prediction::StateDomain::PlayerKinematics));
    CHECK(absorption.expands_ownership);
    CHECK_FALSE(split_merge.implemented);

    World world;
    REQUIRE(world.spawn_player(PlayerOwnerId{0}).has_value());
    const auto unsupported = dots::prediction::build_prediction_scope(
        world.checkpoint(),
        {
            .profile = PredictionProfile::InteractionClosure,
            .mechanics = dots::prediction::mechanic_bit(PredictionMechanic::SplitMerge),
            .owned_owner_ids = {PlayerOwnerId{0}},
            .replay_horizon = mycore::time::TickDelta{1},
            .scope_epoch = mycore::rollback::ScopeEpoch{1},
            .coverage = {},
        });
    REQUIRE(std::get<dots::prediction::ScopeBuildError>(unsupported) ==
            dots::prediction::ScopeBuildError::UnsupportedMechanic);
}

TEST_CASE("Dots prediction profiles construct causal islands and safe fallbacks",
          "[dots][prediction][scope]") {
    World world;
    const auto owned = world.spawn_player(PlayerOwnerId{0}, {});
    const auto remote_player = world.spawn_player(PlayerOwnerId{1}, {8.2F, 0.0F});
    const auto distant_player = world.spawn_player(PlayerOwnerId{2}, {80.0F, 0.0F});
    const auto nearby_food = world.spawn_food({0.0F, 5.0F});
    const auto distant_food = world.spawn_food({80.0F, 5.0F});
    REQUIRE(owned.has_value());
    REQUIRE(remote_player.has_value());
    REQUIRE(distant_player.has_value());
    REQUIRE(nearby_food.has_value());
    REQUIRE(distant_food.has_value());
    const auto checkpoint = world.checkpoint();

    const auto closure = require_scope(checkpoint,
                                       PredictionProfile::InteractionClosure,
                                       dots::prediction::kCurrentPredictionMechanics,
                                       {PlayerOwnerId{0}});
    CHECK(closure.owner_ids == std::vector{PlayerOwnerId{0}, PlayerOwnerId{1}});
    CHECK(closure.player_ids == std::vector{*owned, *remote_player});
    CHECK(closure.food_ids == std::vector{*nearby_food});
    CHECK(dots::prediction::includes_causal_channel(
        closure.required_causal_channels, dots::prediction::CausalChannel::RemoteMovement));

    const auto full = require_scope(checkpoint,
                                    PredictionProfile::FullReplicated,
                                    dots::prediction::kCurrentPredictionMechanics,
                                    {PlayerOwnerId{0}});
    CHECK(full.owner_ids.size() == 3);
    CHECK(full.player_ids.size() == 3);
    CHECK(full.food_ids.size() == 2);

    const auto owned_only = require_scope(checkpoint,
                                          PredictionProfile::OwnedMovement,
                                          dots::prediction::kCurrentPredictionMechanics,
                                          {PlayerOwnerId{0}});
    CHECK(owned_only.active_profile == PredictionProfile::OwnedMovement);
    CHECK(owned_only.requested_mechanics == dots::prediction::kCurrentPredictionMechanics);
    CHECK(owned_only.mechanics == dots::prediction::mechanic_bit(PredictionMechanic::Movement));
    CHECK(owned_only.owner_ids == std::vector{PlayerOwnerId{0}});
    CHECK(owned_only.player_ids == std::vector{*owned});
    CHECK(owned_only.food_ids.empty());

    auto incomplete_coverage = dots::prediction::AuthorityCoverage{};
    incomplete_coverage.available_domains &=
        ~dots::prediction::state_domain_bit(dots::prediction::StateDomain::FoodState);
    const auto fallback = require_scope(checkpoint,
                                        PredictionProfile::InteractionClosure,
                                        dots::prediction::kCurrentPredictionMechanics,
                                        {PlayerOwnerId{0}},
                                        mycore::time::TickDelta{1},
                                        incomplete_coverage);
    CHECK(fallback.active_profile == PredictionProfile::OwnedMovement);
    CHECK(fallback.fallback_reason ==
          dots::prediction::PredictionFallbackReason::IncompleteClosure);

    auto no_remote_causes = dots::prediction::AuthorityCoverage{};
    no_remote_causes.available_causal_channels &=
        ~dots::prediction::causal_channel_bit(dots::prediction::CausalChannel::RemoteMovement);
    const auto causal_fallback = require_scope(checkpoint,
                                               PredictionProfile::InteractionClosure,
                                               dots::prediction::kCurrentPredictionMechanics,
                                               {PlayerOwnerId{0}},
                                               mycore::time::TickDelta{1},
                                               no_remote_causes);
    CHECK(causal_fallback.active_profile == PredictionProfile::OwnedMovement);

    auto missing_owned_state = dots::prediction::AuthorityCoverage{};
    missing_owned_state.available_domains &=
        ~dots::prediction::state_domain_bit(dots::prediction::StateDomain::OwnerCommands);
    const auto unavailable = dots::prediction::build_prediction_scope(
        checkpoint,
        {
            .profile = PredictionProfile::InteractionClosure,
            .mechanics = dots::prediction::kCurrentPredictionMechanics,
            .owned_owner_ids = {PlayerOwnerId{0}},
            .replay_horizon = mycore::time::TickDelta{1},
            .scope_epoch = mycore::rollback::ScopeEpoch{1},
            .coverage = missing_owned_state,
        });
    CHECK(std::get<dots::prediction::ScopeBuildError>(unavailable) ==
          dots::prediction::ScopeBuildError::IncompleteOwnedState);
}

TEST_CASE("Dots scope projection rejects incompatible or out-of-scope authority",
          "[dots][prediction][scope]") {
    World world;
    const auto owned = world.spawn_player(PlayerOwnerId{0});
    REQUIRE(owned.has_value());
    REQUIRE(world.spawn_food({40.0F, 0.0F}).has_value());
    const auto checkpoint = world.checkpoint();
    const auto owned_scope = require_scope(checkpoint,
                                           PredictionProfile::OwnedMovement,
                                           dots::prediction::kCurrentPredictionMechanics,
                                           {PlayerOwnerId{0}});
    const auto projected = require_projection(checkpoint, owned_scope);
    CHECK(projected.players.size() == 1);
    CHECK(projected.food.empty());

    auto incompatible = checkpoint;
    incompatible.rules.player_speed_units_per_second += 1.0F;
    const auto incompatible_result =
        dots::prediction::project_checkpoint(incompatible, owned_scope);
    REQUIRE(std::get<dots::prediction::PredictionError>(incompatible_result).code ==
            dots::prediction::PredictionErrorCode::IncompatibleRules);

    const auto full_scope = require_scope(checkpoint,
                                          PredictionProfile::FullReplicated,
                                          dots::prediction::kCurrentPredictionMechanics,
                                          {PlayerOwnerId{0}});
    REQUIRE(world.spawn_food({80.0F, 0.0F}).has_value());
    const auto expanded_result =
        dots::prediction::project_checkpoint(world.checkpoint(), full_scope);
    REQUIRE(std::get<dots::prediction::PredictionError>(expanded_result).code ==
            dots::prediction::PredictionErrorCode::CheckpointOutsideScope);

    auto invalid_scope = full_scope;
    invalid_scope.scope_epoch = mycore::rollback::ScopeEpoch::invalid();
    const auto invalid_scope_result =
        dots::prediction::project_checkpoint(checkpoint, invalid_scope);
    REQUIRE(std::get<dots::prediction::PredictionError>(invalid_scope_result).code ==
            dots::prediction::PredictionErrorCode::InvalidScope);
}

TEST_CASE("Dots checkpoint digest and typed differences cover complete gameplay state",
          "[dots][prediction][digest]") {
    World previous;
    const auto player = previous.spawn_player(PlayerOwnerId{0});
    const auto food = previous.spawn_food({4.9F, 0.0F});
    REQUIRE(player.has_value());
    REQUIRE(food.has_value());
    const auto checkpoint = previous.checkpoint();
    const auto digest = dots::prediction::checkpoint_digest(checkpoint);
    CHECK(digest.value == 5'969'761'234'082'530'382ULL);
    CHECK(digest == dots::prediction::checkpoint_digest(checkpoint));

    auto changed_checkpoint = checkpoint;
    changed_checkpoint.players.front().position.x += 2.0F;
    changed_checkpoint.players.front().mass += 1.0F;
    changed_checkpoint.food.clear();
    World current;
    REQUIRE_FALSE(current.restore(changed_checkpoint).has_value());
    CHECK(dots::prediction::checkpoint_digest(checkpoint) !=
          dots::prediction::checkpoint_digest(changed_checkpoint));

    const auto scope = require_scope(checkpoint,
                                     PredictionProfile::FullReplicated,
                                     dots::prediction::kCurrentPredictionMechanics,
                                     {PlayerOwnerId{0}});
    const auto difference = dots::prediction::WorldModel{}.diff(previous, current, scope);
    CHECK(difference.structural_change);
    CHECK(difference.maximum_position_delta == Catch::Approx(2.0F));
    CHECK(difference.maximum_mass_delta == Catch::Approx(1.0F));
    REQUIRE(difference.players.size() == 1);
    CHECK(difference.players.front().entity_id == *player);
    REQUIRE(difference.food.size() == 1);
    CHECK(difference.food.front().entity_id == *food);
    CHECK(difference.food.front().previous.has_value());
    CHECK_FALSE(difference.food.front().current.has_value());
}

TEST_CASE("Dots rollback restores authority and rolls retained movement to the current head",
          "[dots][prediction][rollback]") {
    World initial;
    const auto player = initial.spawn_player(PlayerOwnerId{0});
    REQUIRE(player.has_value());
    const auto checkpoint = initial.checkpoint();
    const auto scope = require_scope(checkpoint,
                                     PredictionProfile::FullReplicated,
                                     dots::prediction::mechanic_bit(PredictionMechanic::Movement),
                                     {PlayerOwnerId{0}});
    auto timeline = initialized_timeline(checkpoint, scope);

    REQUIRE(std::holds_alternative<Commit>(
        timeline.advance(mycore::rollback::CommandSequence{0},
                         TickStimulus{.commands = {input(PlayerOwnerId{0}, 0, {1.0F, 0.0F})}})));
    REQUIRE(std::holds_alternative<Commit>(
        timeline.advance(mycore::rollback::CommandSequence{1},
                         TickStimulus{.commands = {input(PlayerOwnerId{0}, 1, {1.0F, 0.0F})}})));
    REQUIRE(std::holds_alternative<Commit>(
        timeline.advance(mycore::rollback::CommandSequence{2},
                         TickStimulus{.commands = {input(PlayerOwnerId{0}, 2, {1.0F, 0.0F})}})));

    World authority;
    REQUIRE_FALSE(authority.restore(checkpoint).has_value());
    REQUIRE(std::holds_alternative<dots::simulation::TickJournal>(
        authority.advance(input(PlayerOwnerId{0}, 0, {0.0F, 1.0F}))));
    World expected;
    REQUIRE_FALSE(expected.restore(authority.checkpoint()).has_value());
    REQUIRE(std::holds_alternative<dots::simulation::TickJournal>(
        expected.advance(input(PlayerOwnerId{0}, 1, {1.0F, 0.0F}))));
    REQUIRE(std::holds_alternative<dots::simulation::TickJournal>(
        expected.advance(input(PlayerOwnerId{0}, 2, {1.0F, 0.0F}))));

    const auto& reconciled =
        require_commit(timeline.reconcile(authority_frame(authority.checkpoint(), scope, 0)));
    CHECK(reconciled.replayed_frame_count == 2);
    CHECK(reconciled.state_diff.maximum_position_delta > 0.0F);
    CHECK(timeline.state()->checkpoint() == expected.checkpoint());
    CHECK(timeline.predicted_tick() == mycore::time::Tick{3});
}

TEST_CASE("Dots rollback retracts predicted food and absorption events",
          "[dots][prediction][rollback][events]") {
    SECTION("food consumption") {
        World initial;
        REQUIRE(initial.spawn_player(PlayerOwnerId{0}).has_value());
        const auto food = initial.spawn_food({4.9F, 0.0F});
        REQUIRE(food.has_value());
        const auto checkpoint = initial.checkpoint();
        const auto scope = require_scope(checkpoint,
                                         PredictionProfile::FullReplicated,
                                         dots::prediction::kCurrentPredictionMechanics,
                                         {PlayerOwnerId{0}});
        auto timeline = initialized_timeline(checkpoint, scope);
        const auto& predicted = require_commit(
            timeline.advance(mycore::rollback::CommandSequence{0},
                             TickStimulus{.commands = {input(PlayerOwnerId{0}, 0)}}));
        CHECK(timeline.state()->food_count() == 0);
        REQUIRE(predicted.event_changes.size() == 1);
        CHECK(predicted.event_changes.front().transition ==
              mycore::rollback::EventTransition::FirstPredicted);

        auto corrected_checkpoint = checkpoint;
        corrected_checkpoint.food.front().position = {40.0F, 0.0F};
        World authority;
        REQUIRE_FALSE(authority.restore(corrected_checkpoint).has_value());
        REQUIRE(std::holds_alternative<dots::simulation::TickJournal>(
            authority.advance(input(PlayerOwnerId{0}, 0))));
        const auto& reconciled =
            require_commit(timeline.reconcile(authority_frame(authority.checkpoint(), scope, 0)));
        REQUIRE(reconciled.event_changes.size() == 1);
        CHECK(reconciled.event_changes.front().transition ==
              mycore::rollback::EventTransition::Retracted);
        CHECK(timeline.state()->food_count() == 1);
        CHECK(reconciled.state_diff.structural_change);
    }

    SECTION("player absorption") {
        World source;
        const auto absorber = source.spawn_player(PlayerOwnerId{0});
        const auto victim = source.spawn_player(PlayerOwnerId{1});
        REQUIRE(absorber.has_value());
        REQUIRE(victim.has_value());
        auto checkpoint = source.checkpoint();
        checkpoint.players.front().mass = 17.0F;
        World initial;
        REQUIRE_FALSE(initial.restore(checkpoint).has_value());
        const auto scope = require_scope(checkpoint,
                                         PredictionProfile::FullReplicated,
                                         dots::prediction::kCurrentPredictionMechanics,
                                         {PlayerOwnerId{0}});
        auto timeline = initialized_timeline(checkpoint, scope);
        const auto& predicted = require_commit(timeline.advance(
            mycore::rollback::CommandSequence{0},
            TickStimulus{
                .commands = {input(PlayerOwnerId{0}, 0)},
                .remote_movement_assumptions = {remote(PlayerOwnerId{1}, checkpoint.tick)},
            }));
        CHECK(timeline.state()->player_count() == 1);
        REQUIRE(predicted.event_changes.size() == 1);

        auto corrected_checkpoint = checkpoint;
        corrected_checkpoint.players.front().mass = 16.0F;
        World authority;
        REQUIRE_FALSE(authority.restore(corrected_checkpoint).has_value());
        const std::vector authority_commands{
            input(PlayerOwnerId{0}, 0),
            TickCommand{
                .type = TickCommandType::AssumeMovement,
                .input_id = InputCommandId::invalid(),
                .owner_id = PlayerOwnerId{1},
                .movement = {},
            },
        };
        REQUIRE(std::holds_alternative<dots::simulation::TickJournal>(
            authority.advance(authority_commands)));
        const auto& reconciled =
            require_commit(timeline.reconcile(authority_frame(authority.checkpoint(), scope, 0)));
        REQUIRE(reconciled.event_changes.size() == 1);
        CHECK(reconciled.event_changes.front().transition ==
              mycore::rollback::EventTransition::Retracted);
        CHECK(timeline.state()->player_count() == 2);
        CHECK(timeline.state()->contains(*victim));
    }
}

TEST_CASE("Dots prediction replays explicit remote assumptions and selected mechanics",
          "[dots][prediction][stimulus]") {
    World world;
    REQUIRE(world.spawn_player(PlayerOwnerId{0}).has_value());
    REQUIRE(world.spawn_player(PlayerOwnerId{1}).has_value());
    REQUIRE(world.spawn_food({}).has_value());
    const auto checkpoint = world.checkpoint();
    const auto scope = require_scope(checkpoint,
                                     PredictionProfile::FullReplicated,
                                     dots::prediction::mechanic_bit(PredictionMechanic::Movement),
                                     {PlayerOwnerId{0}});
    auto timeline = initialized_timeline(checkpoint, scope);

    const auto missing_remote =
        timeline.advance(mycore::rollback::CommandSequence{0},
                         TickStimulus{.commands = {input(PlayerOwnerId{0}, 0)}});
    const auto* failure = std::get_if<dots::prediction::TimelineFailure>(&missing_remote);
    REQUIRE(failure != nullptr);
    REQUIRE(failure->model_error.has_value());
    CHECK(failure->model_error->code == dots::prediction::PredictionErrorCode::InvalidStimulus);

    const auto& advanced =
        require_commit(timeline.advance(mycore::rollback::CommandSequence{0},
                                        TickStimulus{
                                            .commands = {input(PlayerOwnerId{0}, 0)},
                                            .remote_movement_assumptions = {remote(
                                                PlayerOwnerId{1}, checkpoint.tick, {1.0F, 0.0F})},
                                        }));
    CHECK(advanced.event_changes.empty());
    CHECK(timeline.state()->player_count() == 2);
    CHECK(timeline.state()->food_count() == 1);
}

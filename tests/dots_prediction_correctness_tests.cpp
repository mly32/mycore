#include "dots/prediction/prediction.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace {

using dots::prediction::PredictionError;
using dots::prediction::PredictionErrorCode;
using dots::prediction::PredictionProfile;
using dots::prediction::PredictionRequest;
using dots::prediction::PredictionScope;
using dots::prediction::TickStimulus;
using dots::prediction::WorldModel;
using dots::simulation::InputCommandId;
using dots::simulation::PlayerOwnerId;
using dots::simulation::TickCommand;
using dots::simulation::TickCommandType;
using dots::simulation::World;
using dots::simulation::WorldCheckpoint;
using mycore::math::Vector2;

[[nodiscard]] PredictionScope require_scope(const WorldCheckpoint& checkpoint,
                                            PredictionProfile profile,
                                            mycore::time::TickDelta horizon) {
    const auto built = dots::prediction::build_prediction_scope(
        checkpoint,
        PredictionRequest{
            .profile = profile,
            .mechanics = dots::prediction::kCurrentPredictionMechanics,
            .owned_owner_ids = {PlayerOwnerId{0}},
            .subscribed_event_owner_ids = {PlayerOwnerId{0}},
            .replay_horizon = horizon,
            .scope_epoch = mycore::rollback::ScopeEpoch{1},
            .coverage = {},
        });
    const auto* scope = std::get_if<PredictionScope>(&built);
    REQUIRE(scope != nullptr);
    return *scope;
}

[[nodiscard]] WorldCheckpoint require_projection(const WorldCheckpoint& checkpoint,
                                                 const PredictionScope& scope) {
    const auto projected = dots::prediction::project_checkpoint(checkpoint, scope);
    const auto* value = std::get_if<WorldCheckpoint>(&projected);
    REQUIRE(value != nullptr);
    return *value;
}

[[nodiscard]] World require_restore(const WorldCheckpoint& checkpoint,
                                    const PredictionScope& scope) {
    auto restored = WorldModel{}.restore(checkpoint, scope);
    auto* state = std::get_if<World>(&restored);
    REQUIRE(state != nullptr);
    return std::move(*state);
}

[[nodiscard]] TickStimulus stimulus_for(const WorldCheckpoint& checkpoint,
                                        const PredictionScope& scope,
                                        std::uint32_t input_id,
                                        Vector2 movement,
                                        bool split_requested) {
    TickStimulus result{
        .commands = {{
            .type = TickCommandType::ApplyInput,
            .input_id = InputCommandId{input_id},
            .owner_id = PlayerOwnerId{0},
            .movement = movement,
            .split_requested = split_requested,
        }},
        .remote_movement_assumptions = {},
    };
    for (const auto& owner : checkpoint.owners) {
        if (owner.owner_id == PlayerOwnerId{0} ||
            !std::binary_search(scope.owner_ids.begin(), scope.owner_ids.end(), owner.owner_id)) {
            continue;
        }
        result.remote_movement_assumptions.push_back({
            .owner_id = owner.owner_id,
            .source_tick = checkpoint.tick,
            .movement = {},
        });
    }
    return result;
}

[[nodiscard]] std::vector<dots::simulation::SimulationEvent>
require_step(World& state, const TickStimulus& stimulus, const PredictionScope& scope) {
    auto stepped = WorldModel{}.step(state, stimulus, scope);
    auto* events = std::get_if<std::vector<dots::simulation::SimulationEvent>>(&stepped);
    REQUIRE(events != nullptr);
    return std::move(*events);
}

struct Scenario {
    float owned_mass{32.0F};
    float first_remote_mass{16.0F};
    float second_remote_mass{16.0F};
    float first_remote_x{10.0F};
    float second_remote_x{80.0F};
    float first_food_x{5.0F};
    float second_food_x{90.0F};
    std::uint64_t horizon{1};
    bool split{};
};

[[nodiscard]] WorldCheckpoint checkpoint_for(const Scenario& scenario) {
    World world;
    REQUIRE(world.spawn_player(PlayerOwnerId{0}, {}).has_value());
    REQUIRE(world.spawn_player(PlayerOwnerId{1}, {scenario.first_remote_x, 0.0F}).has_value());
    REQUIRE(world.spawn_player(PlayerOwnerId{2}, {scenario.second_remote_x, 0.0F}).has_value());
    REQUIRE(world.spawn_food({scenario.first_food_x, 0.0F}).has_value());
    REQUIRE(world.spawn_food({scenario.second_food_x, 0.0F}).has_value());
    auto checkpoint = world.checkpoint();
    for (auto& player : checkpoint.players) {
        if (player.owner_id == PlayerOwnerId{0}) {
            player.mass = scenario.owned_mass;
        } else if (player.owner_id == PlayerOwnerId{1}) {
            player.mass = scenario.first_remote_mass;
        } else {
            player.mass = scenario.second_remote_mass;
        }
    }
    return checkpoint;
}

void require_closure_matches_full(const Scenario& scenario, std::size_t case_index) {
    INFO("prediction closure differential case " << case_index);
    const auto checkpoint = checkpoint_for(scenario);
    const auto horizon = mycore::time::TickDelta{scenario.horizon};
    const auto full_scope = require_scope(checkpoint, PredictionProfile::FullReplicated, horizon);
    const auto closure_scope =
        require_scope(checkpoint, PredictionProfile::InteractionClosure, horizon);
    auto full_state = require_restore(checkpoint, full_scope);
    auto closure_state =
        require_restore(require_projection(checkpoint, closure_scope), closure_scope);

    for (auto tick = std::uint64_t{}; tick < scenario.horizon; ++tick) {
        INFO("prediction closure differential tick " << tick);
        const auto movement = tick % 2U == 0U ? Vector2{1.0F, 0.0F} : Vector2{0.0F, 1.0F};
        const auto full_checkpoint = full_state.checkpoint();
        const auto closure_checkpoint = closure_state.checkpoint();
        const auto full_events = require_step(full_state,
                                              stimulus_for(full_checkpoint,
                                                           full_scope,
                                                           static_cast<std::uint32_t>(tick),
                                                           movement,
                                                           scenario.split && tick == 0U),
                                              full_scope);
        const auto closure_events = require_step(closure_state,
                                                 stimulus_for(closure_checkpoint,
                                                              closure_scope,
                                                              static_cast<std::uint32_t>(tick),
                                                              movement,
                                                              scenario.split && tick == 0U),
                                                 closure_scope);

        CHECK(closure_events == full_events);
        CHECK(closure_state.checkpoint() ==
              require_projection(full_state.checkpoint(), closure_scope));
    }
}

class DeterministicGenerator {
public:
    [[nodiscard]] std::uint32_t next() noexcept {
        state_ = state_ * 1'664'525U + 1'013'904'223U;
        return state_;
    }

    [[nodiscard]] float quantized(float base, std::uint32_t steps) noexcept {
        return base + static_cast<float>(next() % steps) * 0.5F;
    }

private:
    std::uint32_t state_{0xC105'ED5U};
};

} // namespace

TEST_CASE("Dots prediction scope validation is complete at every public model boundary",
          "[dots][prediction][scope][contract]") {
    const auto checkpoint = checkpoint_for(Scenario{});
    const auto valid = require_scope(
        checkpoint, PredictionProfile::InteractionClosure, mycore::time::TickDelta{4});
    REQUIRE(dots::prediction::is_valid_prediction_scope(valid));

    std::vector<PredictionScope> invalid;
    const auto add = [&invalid, &valid](auto mutate) {
        auto candidate = valid;
        mutate(candidate);
        invalid.push_back(std::move(candidate));
    };
    add([](auto& scope) {
        scope.requested_profile = static_cast<PredictionProfile>(255);
    });
    add([](auto& scope) {
        scope.active_profile = static_cast<PredictionProfile>(255);
    });
    add([](auto& scope) {
        scope.fallback_reason = static_cast<dots::prediction::PredictionFallbackReason>(255);
    });
    add([](auto& scope) {
        scope.scope_epoch = mycore::rollback::ScopeEpoch::invalid();
    });
    add([](auto& scope) {
        scope.replay_horizon = mycore::time::TickDelta{};
    });
    add([](auto& scope) {
        scope.replay_horizon = mycore::time::TickDelta{1'025};
    });
    add([](auto& scope) {
        scope.requested_mechanics = 0;
    });
    add([](auto& scope) {
        scope.mechanics = 0;
    });
    add([](auto& scope) {
        scope.required_domains ^=
            dots::prediction::state_domain_bit(dots::prediction::StateDomain::FoodState);
    });
    add([](auto& scope) {
        scope.required_causal_channels ^=
            dots::prediction::causal_channel_bit(dots::prediction::CausalChannel::RemoteMovement);
    });
    add([](auto& scope) {
        scope.owned_owner_ids.push_back(scope.owned_owner_ids.front());
    });
    add([](auto& scope) {
        scope.subscribed_event_owner_ids = {PlayerOwnerId{99}};
    });
    add([](auto& scope) {
        scope.owner_ids.erase(scope.owner_ids.begin());
    });
    add([](auto& scope) {
        scope.mechanics =
            dots::prediction::mechanic_bit(dots::prediction::PredictionMechanic::Movement);
    });

    for (auto index = std::size_t{}; index < invalid.size(); ++index) {
        INFO("invalid prediction scope case " << index);
        CHECK_FALSE(dots::prediction::is_valid_prediction_scope(invalid[index]));
        const auto projected = dots::prediction::project_checkpoint(checkpoint, invalid[index]);
        REQUIRE(std::holds_alternative<PredictionError>(projected));
        CHECK(std::get<PredictionError>(projected).code == PredictionErrorCode::InvalidScope);
        const auto restored = WorldModel{}.restore(checkpoint, invalid[index]);
        REQUIRE(std::holds_alternative<PredictionError>(restored));
        CHECK(std::get<PredictionError>(restored).code == PredictionErrorCode::InvalidScope);
        auto state = require_restore(require_projection(checkpoint, valid), valid);
        const auto stepped = WorldModel{}.step(state, TickStimulus{}, invalid[index]);
        REQUIRE(std::holds_alternative<PredictionError>(stepped));
        CHECK(std::get<PredictionError>(stepped).code == PredictionErrorCode::InvalidScope);
    }
}

TEST_CASE("Interaction closure matches a full-world prediction oracle",
          "[dots][prediction][scope][differential]") {
    const std::vector targeted{
        Scenario{.first_remote_x = 8.0F, .second_remote_x = 80.0F, .horizon = 1},
        Scenario{.first_remote_x = 9.0F, .second_remote_x = 18.0F, .horizon = 4},
        Scenario{.owned_mass = 16.0F,
                 .first_remote_x = 13.0F,
                 .second_remote_x = 24.0F,
                 .first_food_x = 4.0F,
                 .second_food_x = 9.0F,
                 .horizon = 6},
        Scenario{.owned_mass = 64.0F,
                 .first_remote_x = 20.0F,
                 .second_remote_x = 60.0F,
                 .horizon = 8,
                 .split = true},
    };
    auto case_index = std::size_t{};
    for (const auto& scenario : targeted) {
        require_closure_matches_full(scenario, case_index++);
    }

    DeterministicGenerator generator;
    for (auto generated = std::size_t{}; generated < 64; ++generated) {
        const auto first_remote_x = generator.quantized(6.0F, 40);
        const auto scenario = Scenario{
            .owned_mass = generated % 4U == 0U ? 64.0F : 32.0F,
            .first_remote_mass = generated % 3U == 0U ? 32.0F : 16.0F,
            .second_remote_mass = generated % 5U == 0U ? 32.0F : 16.0F,
            .first_remote_x = first_remote_x,
            .second_remote_x = first_remote_x + generator.quantized(6.0F, 40),
            .first_food_x = generator.quantized(2.0F, 30),
            .second_food_x = generator.quantized(45.0F, 80),
            .horizon = 1U + (generator.next() % 8U),
            .split = generated % 4U == 0U,
        };
        require_closure_matches_full(scenario, case_index++);
    }
}

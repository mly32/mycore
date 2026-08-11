#include "dots/prediction/model.hpp"

#include "dots/prediction/mechanics.hpp"
#include "dots/simulation/movement.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace dots::prediction {
namespace {

template <class Id> [[nodiscard]] bool contains(const std::vector<Id>& values, Id value) {
    return std::binary_search(values.begin(), values.end(), value);
}

[[nodiscard]] PredictionError make_error(PredictionErrorCode code) {
    return PredictionError{.code = code, .checkpoint_error = {}, .tick_error = {}};
}

class DigestWriter {
public:
    void byte(std::uint8_t value) noexcept {
        value_ ^= value;
        value_ *= kPrime;
    }

    template <std::unsigned_integral Value> void unsigned_value(Value value) noexcept {
        for (auto byte_index = sizeof(Value); byte_index > 0; --byte_index) {
            const auto shift = (byte_index - 1U) * 8U;
            byte(static_cast<std::uint8_t>((value >> shift) & Value{0xFF}));
        }
    }

    void float_value(float value) noexcept {
        unsigned_value(std::bit_cast<std::uint32_t>(value));
    }

    void vector(mycore::math::Vector2 value) noexcept {
        float_value(value.x);
        float_value(value.y);
    }

    template <class Id> void id(Id value) noexcept {
        unsigned_value(value.value());
    }

    [[nodiscard]] StateDigest finish() const noexcept {
        return StateDigest{.value = value_};
    }

private:
    static constexpr auto kOffsetBasis = std::uint64_t{14'695'981'039'346'656'037ULL};
    static constexpr auto kPrime = std::uint64_t{1'099'511'628'211};
    std::uint64_t value_{kOffsetBasis};
};

void write_rules(DigestWriter& writer, const simulation::WorldRules& rules) noexcept {
    writer.float_value(rules.initial_player_mass);
    writer.float_value(rules.food_mass);
    writer.float_value(rules.spatial_grid_cell_size);
    writer.float_value(rules.player_speed_units_per_second);
    writer.unsigned_value(rules.split_recast_ticks);
    writer.unsigned_value(rules.merge_delay_ticks);
    writer.unsigned_value(rules.maximum_pieces_per_owner);
    writer.float_value(rules.minimum_split_mass);
    writer.float_value(rules.child_launch_speed_units_per_second);
    writer.float_value(rules.launch_decay_units_per_second_squared);
    writer.float_value(rules.cohesion_speed_units_per_second);
}

[[nodiscard]] float position_delta(mycore::math::Vector2 previous,
                                   mycore::math::Vector2 current) noexcept {
    return mycore::math::length(current - previous);
}

[[nodiscard]] bool valid_owned_command_shape(const simulation::TickCommand& command) noexcept {
    switch (command.type) {
    case simulation::TickCommandType::ApplyInput:
        return command.input_id.is_valid() && std::isfinite(command.movement.x) &&
               std::isfinite(command.movement.y);
    case simulation::TickCommandType::StopMovement:
        return !command.input_id.is_valid() && command.movement == mycore::math::Vector2{} &&
               !command.split_requested;
    case simulation::TickCommandType::AssumeMovement:
    default:
        return false;
    }
}

template <class Value, class Id, class GetId, class AddDifference>
void collect_differences(const std::vector<Value>& previous,
                         const std::vector<Value>& current,
                         GetId get_id,
                         AddDifference add_difference) {
    auto previous_index = std::size_t{};
    auto current_index = std::size_t{};
    while (previous_index < previous.size() || current_index < current.size()) {
        if (current_index == current.size()) {
            add_difference(
                get_id(previous[previous_index]), previous[previous_index], std::optional<Value>{});
            ++previous_index;
            continue;
        }
        if (previous_index == previous.size()) {
            add_difference(
                get_id(current[current_index]), std::optional<Value>{}, current[current_index]);
            ++current_index;
            continue;
        }
        const auto previous_id = get_id(previous[previous_index]);
        const auto current_id = get_id(current[current_index]);
        if (previous_id < current_id) {
            add_difference(previous_id, previous[previous_index], std::optional<Value>{});
            ++previous_index;
        } else if (current_id < previous_id) {
            add_difference(current_id, std::optional<Value>{}, current[current_index]);
            ++current_index;
        } else {
            if (previous[previous_index] != current[current_index]) {
                add_difference(previous_id, previous[previous_index], current[current_index]);
            }
            ++previous_index;
            ++current_index;
        }
    }
}

} // namespace

StateDigest checkpoint_digest(const simulation::WorldCheckpoint& checkpoint) {
    DigestWriter writer;
    writer.unsigned_value(std::uint32_t{0x444F5453});
    writer.unsigned_value(std::uint16_t{1});
    write_rules(writer, checkpoint.rules);
    writer.unsigned_value(checkpoint.tick.value());
    writer.unsigned_value(checkpoint.next_entity_id);

    writer.unsigned_value(static_cast<std::uint64_t>(checkpoint.owners.size()));
    for (const auto& owner : checkpoint.owners) {
        writer.id(owner.owner_id);
        writer.unsigned_value(static_cast<std::uint64_t>(owner.player_ids.size()));
        for (const auto player_id : owner.player_ids) {
            writer.id(player_id);
        }
        writer.vector(owner.movement);
        writer.vector(owner.last_non_zero_movement);
        writer.id(owner.last_input_id);
        writer.unsigned_value(owner.split_cooldown_end_tick.value());
    }

    writer.unsigned_value(static_cast<std::uint64_t>(checkpoint.players.size()));
    for (const auto& player : checkpoint.players) {
        writer.id(player.entity_id);
        writer.id(player.owner_id);
        writer.vector(player.position);
        writer.float_value(player.mass);
        writer.vector(player.launch_velocity);
        writer.unsigned_value(player.merge_eligible_tick.value());
        writer.byte(player.prediction_key ? std::uint8_t{1} : std::uint8_t{0});
        if (player.prediction_key) {
            writer.id(player.prediction_key->owner_id);
            writer.id(player.prediction_key->input_id);
            writer.unsigned_value(player.prediction_key->child_ordinal);
        }
    }

    writer.unsigned_value(static_cast<std::uint64_t>(checkpoint.food.size()));
    for (const auto& food : checkpoint.food) {
        writer.id(food.entity_id);
        writer.vector(food.position);
    }
    return writer.finish();
}

std::variant<WorldModel::State, WorldModel::Error> WorldModel::restore(const Checkpoint& checkpoint,
                                                                       const Scope& scope) const {
    if (!is_valid_prediction_scope(scope)) {
        return make_error(PredictionErrorCode::InvalidScope);
    }
    if (checkpoint.rules != scope.rules) {
        return make_error(PredictionErrorCode::IncompatibleRules);
    }
    auto projected = project_checkpoint(checkpoint, scope);
    if (const auto* projection_error = std::get_if<PredictionError>(&projected)) {
        return *projection_error;
    }
    if (std::get<simulation::WorldCheckpoint>(projected) != checkpoint) {
        return make_error(PredictionErrorCode::CheckpointOutsideScope);
    }

    simulation::World state;
    if (const auto restore_error = state.restore(checkpoint)) {
        return PredictionError{
            .code = PredictionErrorCode::CheckpointRestoreFailed,
            .checkpoint_error = restore_error,
            .tick_error = {},
        };
    }
    return state;
}

WorldModel::Checkpoint WorldModel::capture(const State& state, const Scope& scope) const {
    static_cast<void>(scope);
    return state.checkpoint();
}

std::variant<std::vector<WorldModel::Event>, WorldModel::Error>
WorldModel::step(State& state, const Stimulus& stimulus, const Scope& scope) const {
    if (!is_valid_prediction_scope(scope) || state.rules() != scope.rules) {
        return make_error(PredictionErrorCode::InvalidScope);
    }

    const auto checkpoint = state.checkpoint();
    std::vector<simulation::TickCommand> commands;
    commands.reserve(stimulus.commands.size() + stimulus.remote_movement_assumptions.size());
    for (auto index = std::size_t{}; index < stimulus.commands.size(); ++index) {
        const auto& command = stimulus.commands[index];
        if (!contains(scope.owned_owner_ids, command.owner_id) ||
            !valid_owned_command_shape(command) ||
            std::any_of(stimulus.commands.begin(),
                        stimulus.commands.begin() + static_cast<std::ptrdiff_t>(index),
                        [&command](const simulation::TickCommand& previous) {
                            return previous.owner_id == command.owner_id;
                        })) {
            return make_error(PredictionErrorCode::InvalidStimulus);
        }
        const auto live_owner = std::lower_bound(
            checkpoint.owners.begin(),
            checkpoint.owners.end(),
            command.owner_id,
            [](const simulation::OwnerCheckpoint& owner, simulation::PlayerOwnerId owner_id) {
                return owner.owner_id < owner_id;
            });
        // Replay keeps the sampled command immutable, but an earlier speculative interaction may
        // have removed its owner. It becomes applicable again if later authority restores them.
        if (live_owner != checkpoint.owners.end() && live_owner->owner_id == command.owner_id) {
            commands.push_back(command);
        }
    }

    std::vector<simulation::PlayerOwnerId> live_remote_owners;
    for (const auto& owner : checkpoint.owners) {
        if (!contains(scope.owned_owner_ids, owner.owner_id)) {
            live_remote_owners.push_back(owner.owner_id);
        }
    }
    if (stimulus.remote_movement_assumptions.size() != live_remote_owners.size()) {
        return make_error(PredictionErrorCode::InvalidStimulus);
    }
    for (auto index = std::size_t{}; index < live_remote_owners.size(); ++index) {
        const auto& assumption = stimulus.remote_movement_assumptions[index];
        if (assumption.owner_id != live_remote_owners[index] ||
            assumption.source_tick > state.tick() ||
            !simulation::is_valid_player_movement(assumption.movement)) {
            return make_error(PredictionErrorCode::InvalidStimulus);
        }
        commands.push_back(simulation::TickCommand{
            .type = simulation::TickCommandType::AssumeMovement,
            .input_id = simulation::InputCommandId::invalid(),
            .owner_id = assumption.owner_id,
            .movement = assumption.movement,
        });
    }

    auto result = state.advance(
        commands,
        simulation::TickMechanics{
            .player_absorption =
                includes_mechanic(scope.mechanics, PredictionMechanic::PlayerAbsorption),
            .food_consumption =
                includes_mechanic(scope.mechanics, PredictionMechanic::FoodConsumption),
            .split_merge = includes_mechanic(scope.mechanics, PredictionMechanic::SplitMerge),
        });
    if (const auto* tick_error = std::get_if<simulation::TickError>(&result)) {
        return PredictionError{
            .code = PredictionErrorCode::TickFailed,
            .checkpoint_error = {},
            .tick_error = *tick_error,
        };
    }
    auto events = std::move(std::get<simulation::TickJournal>(result).events);
    std::erase_if(events, [&scope](const simulation::SimulationEvent& event) {
        const auto participants = simulation::simulation_event_participants(event);
        return std::ranges::none_of(participants.owners(), [&scope](auto owner_id) {
            return contains(scope.subscribed_event_owner_ids, owner_id);
        });
    });
    return events;
}

WorldModel::StateDigest WorldModel::digest(const Checkpoint& checkpoint, const Scope& scope) const {
    static_cast<void>(scope);
    return checkpoint_digest(checkpoint);
}

WorldModel::StateDiff
WorldModel::diff(const State& previous, const State& current, const Scope& scope) const {
    static_cast<void>(scope);
    const auto previous_checkpoint = previous.checkpoint();
    const auto current_checkpoint = current.checkpoint();
    StateDifference result{
        .previous_tick = previous_checkpoint.tick,
        .current_tick = current_checkpoint.tick,
        .rules_changed = previous_checkpoint.rules != current_checkpoint.rules,
        .allocator_changed =
            previous_checkpoint.next_entity_id != current_checkpoint.next_entity_id,
        .structural_change = false,
        .maximum_position_delta = 0.0F,
        .maximum_mass_delta = 0.0F,
        .owners = {},
        .players = {},
        .food = {},
    };

    collect_differences<simulation::OwnerCheckpoint, simulation::PlayerOwnerId>(
        previous_checkpoint.owners,
        current_checkpoint.owners,
        [](const auto& owner) {
            return owner.owner_id;
        },
        [&result](simulation::PlayerOwnerId owner_id,
                  std::optional<simulation::OwnerCheckpoint> old_owner,
                  std::optional<simulation::OwnerCheckpoint> new_owner) {
            result.structural_change = result.structural_change || !old_owner || !new_owner ||
                                       old_owner->player_ids != new_owner->player_ids;
            result.owners.push_back({
                .owner_id = owner_id,
                .previous = std::move(old_owner),
                .current = std::move(new_owner),
            });
        });
    collect_differences<simulation::PlayerCheckpoint, simulation::EntityId>(
        previous_checkpoint.players,
        current_checkpoint.players,
        [](const auto& player) {
            return player.entity_id;
        },
        [&result](simulation::EntityId entity_id,
                  std::optional<simulation::PlayerCheckpoint> old_player,
                  std::optional<simulation::PlayerCheckpoint> new_player) {
            result.structural_change = result.structural_change || !old_player || !new_player;
            if (old_player && new_player) {
                result.maximum_position_delta =
                    std::max(result.maximum_position_delta,
                             position_delta(old_player->position, new_player->position));
                result.maximum_mass_delta = std::max(result.maximum_mass_delta,
                                                     std::abs(old_player->mass - new_player->mass));
            }
            result.players.push_back({
                .entity_id = entity_id,
                .previous = old_player,
                .current = new_player,
            });
        });
    collect_differences<simulation::FoodCheckpoint, simulation::EntityId>(
        previous_checkpoint.food,
        current_checkpoint.food,
        [](const auto& food) {
            return food.entity_id;
        },
        [&result](simulation::EntityId entity_id,
                  std::optional<simulation::FoodCheckpoint> old_food,
                  std::optional<simulation::FoodCheckpoint> new_food) {
            result.structural_change = result.structural_change || !old_food || !new_food;
            if (old_food && new_food) {
                result.maximum_position_delta =
                    std::max(result.maximum_position_delta,
                             position_delta(old_food->position, new_food->position));
            }
            result.food.push_back({
                .entity_id = entity_id,
                .previous = old_food,
                .current = new_food,
            });
        });
    return result;
}

WorldModel::EventKey WorldModel::event_key(const Event& event) const {
    return simulation::simulation_event_key(event);
}

std::size_t
SimulationEventKeyHash::operator()(const simulation::SimulationEventKey& key) const noexcept {
    const auto combine = [](std::size_t seed, std::size_t value) {
        constexpr auto kHashConstant = std::size_t{0x9E3779B9};
        return seed ^ (value + kHashConstant + (seed << 6U) + (seed >> 2U));
    };
    if (const auto* food = std::get_if<simulation::FoodConsumedKey>(&key)) {
        return combine(std::size_t{0}, food->food_entity_id.value());
    }
    if (const auto* absorbed = std::get_if<simulation::PlayerAbsorbedKey>(&key)) {
        return combine(std::size_t{1}, absorbed->victim_entity_id.value());
    }
    if (const auto* split = std::get_if<simulation::PlayerSplitKey>(&key)) {
        auto result = combine(std::size_t{2}, split->owner_id.value());
        result = combine(result, split->input_id.value());
        return combine(result, split->child_ordinal);
    }
    if (const auto* merged = std::get_if<simulation::PiecesMergedKey>(&key)) {
        auto result = combine(std::size_t{3}, merged->first_entity_id.value());
        return combine(result, merged->second_entity_id.value());
    }
    return 0;
}

} // namespace dots::prediction

#include "dots/simulation/world.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace dots::simulation {
namespace {

struct FoodConsumption {
    EntityId food_id;
    EntityId player_id;
    std::size_t player_index{};
};

struct PlayerAbsorptionCandidate {
    std::size_t absorber_index{};
    std::size_t victim_index{};
};

template <typename T> void reserve_for_append(std::vector<T>& values) {
    if (values.size() < values.capacity()) {
        return;
    }
    const auto next_capacity = values.capacity() == 0 ? std::size_t{1} : values.capacity() * 2;
    values.reserve(next_capacity);
}

[[nodiscard]] bool is_finite(mycore::math::Vector2 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

[[nodiscard]] bool is_normalized(mycore::math::Vector2 value) noexcept {
    constexpr float kLengthTolerance = 0.0001F;
    return is_finite(value) && mycore::math::length_squared(value) <= 1.0F + kLengthTolerance;
}

[[nodiscard]] bool valid_rules(const WorldRules& rules) noexcept {
    return std::isfinite(rules.initial_player_mass) && rules.initial_player_mass > 0.0F &&
           std::isfinite(rules.food_mass) && rules.food_mass > 0.0F &&
           std::isfinite(rules.spatial_grid_cell_size) && rules.spatial_grid_cell_size > 0.0F &&
           std::isfinite(rules.player_speed_units_per_second) &&
           rules.player_speed_units_per_second > 0.0F && rules.split_recast_ticks > 0 &&
           rules.merge_delay_ticks > 0 && rules.maximum_pieces_per_owner > 0 &&
           std::isfinite(rules.minimum_split_mass) && rules.minimum_split_mass > 0.0F &&
           std::isfinite(rules.child_launch_speed_units_per_second) &&
           rules.child_launch_speed_units_per_second >= 0.0F &&
           std::isfinite(rules.launch_decay_units_per_second_squared) &&
           rules.launch_decay_units_per_second_squared >= 0.0F &&
           std::isfinite(rules.cohesion_speed_units_per_second) &&
           rules.cohesion_speed_units_per_second >= 0.0F;
}

template <class Range, class Projection>
[[nodiscard]] bool is_strictly_sorted(const Range& values, Projection projection) {
    return std::adjacent_find(values.begin(), values.end(), [&](const auto& lhs, const auto& rhs) {
               return projection(lhs) >= projection(rhs);
           }) == values.end();
}

} // namespace

World::World()
    : World(WorldRules{}) {}

World::World(WorldRules rules)
    : rules_(rules),
      spatial_grid_(rules.spatial_grid_cell_size),
      last_tick_journal_{.tick = tick_, .events = {}} {
    if (!valid_rules(rules_)) {
        throw std::invalid_argument{"Dots World rules must be finite and valid"};
    }
}

std::optional<EntityId> World::spawn_player(PlayerOwnerId owner_id,
                                            mycore::math::Vector2 position,
                                            std::optional<PredictionKey> prediction_key) {
    const auto entity_id = next_entity_id();
    const auto radius = radius_for_mass(rules_.initial_player_mass);
    const Circle bounds{.center = position, .radius = radius};
    if (!owner_id.is_valid() || !entity_id || !spatial_grid_.can_index(bounds) ||
        (prediction_key &&
         (!prediction_key->owner_id.is_valid() || !prediction_key->input_id.is_valid() ||
          prediction_key->owner_id != owner_id))) {
        return std::nullopt;
    }

    reserve_player_capacity();
    if (!spatial_grid_.insert(*entity_id, bounds)) {
        return std::nullopt;
    }

    auto owner = std::lower_bound(owners_.begin(),
                                  owners_.end(),
                                  owner_id,
                                  [](const OwnerCheckpoint& state, PlayerOwnerId id) {
                                      return state.owner_id < id;
                                  });
    if (owner == owners_.end() || owner->owner_id != owner_id) {
        owner = owners_.insert(owner,
                               OwnerCheckpoint{
                                   .owner_id = owner_id,
                                   .player_ids = {},
                                   .movement = {},
                                   .last_non_zero_movement = {},
                                   .last_input_id = InputCommandId::invalid(),
                                   .split_cooldown_end_tick = {},
                               });
    }

    const auto index = entity_ids_.size();
    entity_ids_.push_back(*entity_id);
    owner_ids_.push_back(owner_id);
    positions_.push_back(position);
    masses_.push_back(rules_.initial_player_mass);
    radii_.push_back(radius);
    launch_velocities_.emplace_back();
    merge_eligible_ticks_.emplace_back();
    prediction_keys_.push_back(prediction_key);
    owner->player_ids.push_back(*entity_id);
    entity_locations_.emplace(entity_id->value(),
                              EntityLocation{.kind = EntityKind::Player, .index = index});
    ++next_entity_id_;
    return entity_id;
}

bool World::remove_player(EntityId entity_id) {
    const auto index = find_index(entity_id);
    if (!index) {
        return false;
    }
    const auto removed_owner_id = owner_ids_[*index];

    if (!spatial_grid_.remove(entity_id)) {
        throw std::logic_error{"Player is missing from the Dots spatial grid"};
    }
    const auto last_index = entity_ids_.size() - 1;
    if (*index != last_index) {
        const auto moved_entity_id = entity_ids_[last_index];
        entity_ids_[*index] = entity_ids_[last_index];
        owner_ids_[*index] = owner_ids_[last_index];
        positions_[*index] = positions_[last_index];
        masses_[*index] = masses_[last_index];
        radii_[*index] = radii_[last_index];
        launch_velocities_[*index] = launch_velocities_[last_index];
        merge_eligible_ticks_[*index] = merge_eligible_ticks_[last_index];
        prediction_keys_[*index] = prediction_keys_[last_index];
        entity_locations_[moved_entity_id.value()] =
            EntityLocation{.kind = EntityKind::Player, .index = *index};
    }

    auto* owner = find_owner(removed_owner_id);
    if (owner == nullptr) {
        throw std::logic_error{"Player owner is missing from the Dots World"};
    }
    std::erase(owner->player_ids, entity_id);
    entity_ids_.pop_back();
    owner_ids_.pop_back();
    positions_.pop_back();
    masses_.pop_back();
    radii_.pop_back();
    launch_velocities_.pop_back();
    merge_eligible_ticks_.pop_back();
    prediction_keys_.pop_back();
    entity_locations_.erase(entity_id.value());
    owner = find_owner(removed_owner_id);
    if (owner != nullptr && owner->player_ids.empty()) {
        std::erase_if(owners_, [removed_owner_id](const OwnerCheckpoint& state) {
            return state.owner_id == removed_owner_id;
        });
    }
    return true;
}

std::optional<EntityId> World::spawn_food(mycore::math::Vector2 position) {
    const auto entity_id = next_entity_id();
    const Circle bounds{.center = position, .radius = radius_for_mass(rules_.food_mass)};
    if (!entity_id || !spatial_grid_.can_index(bounds)) {
        return std::nullopt;
    }

    reserve_food_capacity();
    if (!spatial_grid_.insert(*entity_id, bounds)) {
        return std::nullopt;
    }

    const auto index = food_entity_ids_.size();
    food_entity_ids_.push_back(*entity_id);
    food_positions_.push_back(position);
    entity_locations_.emplace(entity_id->value(),
                              EntityLocation{.kind = EntityKind::Food, .index = index});
    ++next_entity_id_;
    return entity_id;
}

bool World::remove_food(EntityId entity_id) {
    const auto index = find_food_index(entity_id);
    if (!index) {
        return false;
    }

    if (!spatial_grid_.remove(entity_id)) {
        throw std::logic_error{"Food is missing from the Dots spatial grid"};
    }
    const auto last_index = food_entity_ids_.size() - 1;
    if (*index != last_index) {
        const auto moved_entity_id = food_entity_ids_[last_index];
        food_entity_ids_[*index] = food_entity_ids_[last_index];
        food_positions_[*index] = food_positions_[last_index];
        entity_locations_[moved_entity_id.value()] =
            EntityLocation{.kind = EntityKind::Food, .index = *index};
    }
    food_entity_ids_.pop_back();
    food_positions_.pop_back();
    entity_locations_.erase(entity_id.value());
    return true;
}

const WorldRules& World::rules() const noexcept {
    return rules_;
}

bool World::contains(EntityId entity_id) const noexcept {
    return find_location(entity_id) != nullptr;
}

std::size_t World::player_count() const noexcept {
    return entity_ids_.size();
}

std::size_t World::food_count() const noexcept {
    return food_entity_ids_.size();
}

std::size_t World::occupied_spatial_cell_count() const noexcept {
    return spatial_grid_.occupied_cell_count();
}

std::span<const EntityId> World::player_ids() const noexcept {
    return entity_ids_;
}

std::span<const EntityId> World::food_ids() const noexcept {
    return food_entity_ids_;
}

std::optional<PlayerOwnerId> World::player_owner(EntityId entity_id) const noexcept {
    const auto index = find_index(entity_id);
    if (!index) {
        return std::nullopt;
    }
    return owner_ids_[*index];
}

std::optional<mycore::math::Vector2> World::position(EntityId entity_id) const noexcept {
    const auto* location = find_location(entity_id);
    if (location == nullptr) {
        return std::nullopt;
    }
    if (location->kind == EntityKind::Player) {
        return positions_[location->index];
    }
    return food_positions_[location->index];
}

std::optional<float> World::mass(EntityId entity_id) const noexcept {
    const auto* location = find_location(entity_id);
    if (location == nullptr) {
        return std::nullopt;
    }
    if (location->kind == EntityKind::Player) {
        return masses_[location->index];
    }
    return rules_.food_mass;
}

std::optional<float> World::radius(EntityId entity_id) const noexcept {
    const auto* location = find_location(entity_id);
    if (location == nullptr) {
        return std::nullopt;
    }
    if (location->kind == EntityKind::Player) {
        return radii_[location->index];
    }
    return radius_for_mass(rules_.food_mass);
}

std::optional<PredictionKey> World::prediction_key(EntityId entity_id) const noexcept {
    const auto index = find_index(entity_id);
    if (!index) {
        return std::nullopt;
    }
    return prediction_keys_[*index];
}

bool World::has_available_entity_id() const noexcept {
    return next_entity_id().has_value();
}

InitialPlayerSpawnStatus
World::classify_initial_player_spawn(mycore::math::Vector2 position) const {
    const Circle candidate_circle{.center = position,
                                  .radius = radius_for_mass(rules_.initial_player_mass)};
    const auto visit_result = spatial_grid_.visit_candidates(
        candidate_circle, [this, candidate_circle](EntityId candidate) {
            const auto player_index = find_index(candidate);
            return !player_index || !circles_overlap(candidate_circle,
                                                     {.center = positions_[*player_index],
                                                      .radius = radii_[*player_index]});
        });

    if (visit_result == SpatialGrid::VisitResult::InvalidBounds) {
        return InitialPlayerSpawnStatus::OutsideRepresentableGrid;
    }
    if (visit_result == SpatialGrid::VisitResult::Stopped) {
        return InitialPlayerSpawnStatus::Blocked;
    }
    return InitialPlayerSpawnStatus::Clear;
}

mycore::time::Tick World::tick() const noexcept {
    return tick_;
}

const TickJournal& World::last_tick_journal() const noexcept {
    return last_tick_journal_;
}

WorldCheckpoint World::checkpoint() const {
    WorldCheckpoint result{
        .rules = rules_,
        .tick = tick_,
        .next_entity_id = next_entity_id_,
        .owners = owners_,
        .players = {},
        .food = {},
    };
    result.players.reserve(entity_ids_.size());
    for (std::size_t index = 0; index < entity_ids_.size(); ++index) {
        result.players.push_back(PlayerCheckpoint{
            .entity_id = entity_ids_[index],
            .owner_id = owner_ids_[index],
            .position = positions_[index],
            .mass = masses_[index],
            .launch_velocity = launch_velocities_[index],
            .merge_eligible_tick = merge_eligible_ticks_[index],
            .prediction_key = prediction_keys_[index],
        });
    }
    std::sort(result.players.begin(),
              result.players.end(),
              [](const PlayerCheckpoint& lhs, const PlayerCheckpoint& rhs) {
                  return lhs.entity_id < rhs.entity_id;
              });

    result.food.reserve(food_entity_ids_.size());
    for (std::size_t index = 0; index < food_entity_ids_.size(); ++index) {
        result.food.push_back(FoodCheckpoint{.entity_id = food_entity_ids_[index],
                                             .position = food_positions_[index]});
    }
    std::sort(result.food.begin(),
              result.food.end(),
              [](const FoodCheckpoint& lhs, const FoodCheckpoint& rhs) {
                  return lhs.entity_id < rhs.entity_id;
              });
    return result;
}

std::optional<CheckpointRestoreError> World::restore(const WorldCheckpoint& checkpoint) {
    if (!valid_rules(checkpoint.rules)) {
        return CheckpointRestoreError::InvalidRules;
    }
    if (!is_strictly_sorted(checkpoint.owners,
                            [](const OwnerCheckpoint& owner) {
                                return owner.owner_id;
                            }) ||
        !is_strictly_sorted(checkpoint.players,
                            [](const PlayerCheckpoint& player) {
                                return player.entity_id;
                            }) ||
        !is_strictly_sorted(checkpoint.food, [](const FoodCheckpoint& food) {
            return food.entity_id;
        })) {
        return CheckpointRestoreError::InvalidOrdering;
    }

    std::map<std::uint32_t, std::vector<EntityId>> expected_owner_pieces;
    std::unordered_set<std::uint32_t> entity_ids;
    for (const auto& owner : checkpoint.owners) {
        if (!owner.owner_id.is_valid() || owner.player_ids.empty() ||
            !is_strictly_sorted(owner.player_ids,
                                [](EntityId entity_id) {
                                    return entity_id;
                                }) ||
            !is_normalized(owner.movement) || !is_normalized(owner.last_non_zero_movement)) {
            return CheckpointRestoreError::InvalidOwnerState;
        }
        expected_owner_pieces.emplace(owner.owner_id.value(), std::vector<EntityId>{});
    }

    const auto entity_precedes_allocator = [&checkpoint](EntityId entity_id) {
        return entity_id.is_valid() && (checkpoint.next_entity_id == EntityId::kInvalidValue ||
                                        entity_id.value() < checkpoint.next_entity_id);
    };
    for (const auto& player : checkpoint.players) {
        const auto owner = expected_owner_pieces.find(player.owner_id.value());
        if (!entity_precedes_allocator(player.entity_id) || !player.owner_id.is_valid() ||
            owner == expected_owner_pieces.end() ||
            !entity_ids.insert(player.entity_id.value()).second || !is_finite(player.position) ||
            !std::isfinite(player.mass) || player.mass <= 0.0F ||
            !is_finite(player.launch_velocity) ||
            (player.prediction_key && (!player.prediction_key->owner_id.is_valid() ||
                                       !player.prediction_key->input_id.is_valid() ||
                                       player.prediction_key->owner_id != player.owner_id))) {
            return CheckpointRestoreError::InvalidEntityState;
        }
        owner->second.push_back(player.entity_id);
    }
    for (const auto& food : checkpoint.food) {
        if (!entity_precedes_allocator(food.entity_id) ||
            !entity_ids.insert(food.entity_id.value()).second || !is_finite(food.position)) {
            return CheckpointRestoreError::InvalidEntityState;
        }
    }
    for (const auto& owner : checkpoint.owners) {
        if (expected_owner_pieces.at(owner.owner_id.value()) != owner.player_ids) {
            return CheckpointRestoreError::InvalidOwnerState;
        }
    }

    World candidate{checkpoint.rules};
    candidate.tick_ = checkpoint.tick;
    candidate.next_entity_id_ = checkpoint.next_entity_id;
    candidate.owners_ = checkpoint.owners;
    candidate.reserve_player_capacity();
    for (const auto& player : checkpoint.players) {
        const auto radius = radius_for_mass(player.mass);
        if (!candidate.spatial_grid_.insert(player.entity_id,
                                            {.center = player.position, .radius = radius})) {
            return CheckpointRestoreError::InvalidGeometry;
        }
        const auto index = candidate.entity_ids_.size();
        candidate.entity_ids_.push_back(player.entity_id);
        candidate.owner_ids_.push_back(player.owner_id);
        candidate.positions_.push_back(player.position);
        candidate.masses_.push_back(player.mass);
        candidate.radii_.push_back(radius);
        candidate.launch_velocities_.push_back(player.launch_velocity);
        candidate.merge_eligible_ticks_.push_back(player.merge_eligible_tick);
        candidate.prediction_keys_.push_back(player.prediction_key);
        candidate.entity_locations_.emplace(
            player.entity_id.value(), EntityLocation{.kind = EntityKind::Player, .index = index});
    }

    candidate.reserve_food_capacity();
    for (const auto& food : checkpoint.food) {
        if (!candidate.spatial_grid_.insert(
                food.entity_id,
                {.center = food.position, .radius = radius_for_mass(candidate.rules_.food_mass)})) {
            return CheckpointRestoreError::InvalidGeometry;
        }
        const auto index = candidate.food_entity_ids_.size();
        candidate.food_entity_ids_.push_back(food.entity_id);
        candidate.food_positions_.push_back(food.position);
        candidate.entity_locations_.emplace(
            food.entity_id.value(), EntityLocation{.kind = EntityKind::Food, .index = index});
    }
    candidate.last_tick_journal_ = TickJournal{.tick = checkpoint.tick, .events = {}};
    *this = std::move(candidate);
    return std::nullopt;
}

std::optional<TickError> World::apply_commands(std::span<const TickCommand> commands,
                                               std::vector<OwnerCheckpoint>& next_owners) const {
    std::vector<TickCommand> ordered_commands{commands.begin(), commands.end()};
    std::sort(ordered_commands.begin(),
              ordered_commands.end(),
              [](const TickCommand& lhs, const TickCommand& rhs) {
                  return lhs.owner_id < rhs.owner_id;
              });

    for (const auto& command : ordered_commands) {
        if (!command.owner_id.is_valid()) {
            return TickError::InvalidCommand;
        }
    }
    if (std::adjacent_find(ordered_commands.begin(),
                           ordered_commands.end(),
                           [](const TickCommand& lhs, const TickCommand& rhs) {
                               return lhs.owner_id == rhs.owner_id;
                           }) != ordered_commands.end()) {
        return TickError::DuplicateOwnerCommand;
    }

    for (const auto& command : ordered_commands) {
        auto owner = std::lower_bound(next_owners.begin(),
                                      next_owners.end(),
                                      command.owner_id,
                                      [](const OwnerCheckpoint& state, PlayerOwnerId id) {
                                          return state.owner_id < id;
                                      });
        if (owner == next_owners.end() || owner->owner_id != command.owner_id) {
            return TickError::InvalidCommand;
        }

        switch (command.type) {
        case TickCommandType::ApplyInput:
            if (!command.input_id.is_valid() || !is_finite(command.movement) ||
                (owner->last_input_id.is_valid() && command.input_id <= owner->last_input_id)) {
                return TickError::InvalidCommand;
            }
            owner->movement = normalized_player_movement(command.movement);
            if (mycore::math::length_squared(owner->movement) > 0.0F) {
                owner->last_non_zero_movement = owner->movement;
            }
            owner->last_input_id = command.input_id;
            break;
        case TickCommandType::StopMovement:
            if (command.input_id.is_valid() || command.movement != mycore::math::Vector2{}) {
                return TickError::InvalidCommand;
            }
            owner->movement = {};
            break;
        default:
            return TickError::InvalidCommand;
        }
    }
    return std::nullopt;
}

TickResult World::advance(std::span<const TickCommand> commands) {
    auto next_owners = owners_;
    if (const auto error = apply_commands(commands, next_owners)) {
        return *error;
    }

    TickJournal journal;
    if (!advance_simulation(std::move(next_owners), journal)) {
        return TickError::SimulationRejected;
    }
    return journal;
}

TickResult World::advance(const TickCommand& command) {
    return advance(std::span{&command, std::size_t{1}});
}

bool World::step() {
    return std::holds_alternative<TickJournal>(advance(std::span<const TickCommand>{}));
}

bool World::advance_simulation(std::vector<OwnerCheckpoint> next_owners, TickJournal& journal) {
    if (tick_.value() == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }

    std::vector<mycore::math::Vector2> next_positions;
    next_positions.reserve(positions_.size());
    for (std::size_t index = 0; index < positions_.size(); ++index) {
        const auto owner = std::lower_bound(next_owners.begin(),
                                            next_owners.end(),
                                            owner_ids_[index],
                                            [](const OwnerCheckpoint& state, PlayerOwnerId id) {
                                                return state.owner_id < id;
                                            });
        if (owner == next_owners.end() || owner->owner_id != owner_ids_[index]) {
            throw std::logic_error{"Player owner is missing from the Dots tick state"};
        }
        const auto next_position = advance_player_position(
            positions_[index], owner->movement, rules_.player_speed_units_per_second);
        if (!spatial_grid_.can_index({.center = next_position, .radius = radii_[index]})) {
            return false;
        }
        next_positions.push_back(next_position);
    }

    SpatialGrid collision_grid{rules_.spatial_grid_cell_size};
    for (std::size_t index = 0; index < entity_ids_.size(); ++index) {
        if (!collision_grid.insert(entity_ids_[index],
                                   {.center = next_positions[index], .radius = radii_[index]})) {
            throw std::logic_error{"Player could not enter the Dots collision-phase grid"};
        }
    }

    std::vector<PlayerAbsorptionCandidate> absorption_candidates;
    for (std::size_t first = 0; first < entity_ids_.size(); ++first) {
        const Circle first_circle{.center = next_positions[first], .radius = radii_[first]};
        for (const auto candidate_id : collision_grid.query(first_circle)) {
            const auto second_index = find_index(candidate_id);
            if (!second_index || *second_index <= first) {
                continue;
            }
            const auto second = *second_index;
            if (owner_ids_[first] == owner_ids_[second]) {
                continue;
            }
            const Circle second_circle{.center = next_positions[second], .radius = radii_[second]};
            if (!circles_overlap(first_circle, second_circle) ||
                masses_[first] == masses_[second]) {
                continue;
            }
            if (masses_[first] > masses_[second]) {
                absorption_candidates.push_back({.absorber_index = first, .victim_index = second});
            } else {
                absorption_candidates.push_back({.absorber_index = second, .victim_index = first});
            }
        }
    }

    std::sort(absorption_candidates.begin(),
              absorption_candidates.end(),
              [this](const PlayerAbsorptionCandidate& lhs, const PlayerAbsorptionCandidate& rhs) {
                  const auto lhs_mass = masses_[lhs.absorber_index];
                  const auto rhs_mass = masses_[rhs.absorber_index];
                  if (lhs_mass != rhs_mass) {
                      return lhs_mass > rhs_mass;
                  }
                  const auto lhs_absorber = entity_ids_[lhs.absorber_index];
                  const auto rhs_absorber = entity_ids_[rhs.absorber_index];
                  if (lhs_absorber != rhs_absorber) {
                      return lhs_absorber < rhs_absorber;
                  }
                  return entity_ids_[lhs.victim_index] < entity_ids_[rhs.victim_index];
              });

    std::vector<bool> live_players(entity_ids_.size(), true);
    std::vector<float> mass_gains(entity_ids_.size());
    std::vector<SimulationEvent> next_events;
    next_events.reserve(absorption_candidates.size());
    auto completed_tick = tick_;
    completed_tick += mycore::time::TickDelta{1};
    for (const auto& candidate : absorption_candidates) {
        if (!live_players[candidate.absorber_index] || !live_players[candidate.victim_index]) {
            continue;
        }
        live_players[candidate.victim_index] = false;
        mass_gains[candidate.absorber_index] += masses_[candidate.victim_index];
        next_events.emplace_back(PlayerAbsorbed{
            .tick = completed_tick,
            .absorber_entity_id = entity_ids_[candidate.absorber_index],
            .victim_entity_id = entity_ids_[candidate.victim_index],
            .absorber_owner_id = owner_ids_[candidate.absorber_index],
            .victim_owner_id = owner_ids_[candidate.victim_index],
            .transferred_mass = masses_[candidate.victim_index],
        });
    }

    std::vector<FoodConsumption> food_consumptions;
    for (std::size_t player_index = 0; player_index < entity_ids_.size(); ++player_index) {
        if (!live_players[player_index]) {
            continue;
        }
        const Circle player_circle{.center = next_positions[player_index],
                                   .radius = radii_[player_index]};
        for (const auto candidate : spatial_grid_.query(player_circle)) {
            const auto food_index = find_food_index(candidate);
            if (!food_index) {
                continue;
            }
            const Circle food_circle{.center = food_positions_[*food_index],
                                     .radius = radius_for_mass(rules_.food_mass)};
            if (circles_overlap(player_circle, food_circle)) {
                food_consumptions.push_back({
                    .food_id = candidate,
                    .player_id = entity_ids_[player_index],
                    .player_index = player_index,
                });
            }
        }
    }
    std::sort(food_consumptions.begin(),
              food_consumptions.end(),
              [](const FoodConsumption& lhs, const FoodConsumption& rhs) {
                  if (lhs.food_id != rhs.food_id) {
                      return lhs.food_id < rhs.food_id;
                  }
                  return lhs.player_id < rhs.player_id;
              });

    std::vector<EntityId> consumed_food_ids;
    auto last_food_id = EntityId::invalid();
    for (const auto& consumption : food_consumptions) {
        if (consumption.food_id == last_food_id) {
            continue;
        }
        last_food_id = consumption.food_id;
        mass_gains[consumption.player_index] += rules_.food_mass;
        consumed_food_ids.push_back(consumption.food_id);
        next_events.emplace_back(FoodConsumed{
            .tick = completed_tick,
            .food_entity_id = consumption.food_id,
            .consumer_entity_id = consumption.player_id,
            .consumer_owner_id = owner_ids_[consumption.player_index],
            .transferred_mass = rules_.food_mass,
        });
    }

    std::vector<float> next_radii = radii_;
    for (std::size_t index = 0; index < entity_ids_.size(); ++index) {
        if (!live_players[index]) {
            continue;
        }
        next_radii[index] = radius_for_mass(masses_[index] + mass_gains[index]);
        if (!spatial_grid_.can_index(
                {.center = next_positions[index], .radius = next_radii[index]})) {
            return false;
        }
    }

    owners_ = std::move(next_owners);
    for (std::size_t index = 0; index < entity_ids_.size(); ++index) {
        if (!live_players[index]) {
            continue;
        }
        if (!spatial_grid_.update(entity_ids_[index],
                                  {.center = next_positions[index], .radius = next_radii[index]})) {
            throw std::logic_error{"Player could not be updated in the Dots spatial grid"};
        }
        positions_[index] = next_positions[index];
        masses_[index] += mass_gains[index];
        radii_[index] = next_radii[index];
    }

    std::vector<EntityId> removed_player_ids;
    removed_player_ids.reserve(entity_ids_.size());
    for (std::size_t index = 0; index < entity_ids_.size(); ++index) {
        if (!live_players[index]) {
            removed_player_ids.push_back(entity_ids_[index]);
        }
    }
    for (const auto entity_id : removed_player_ids) {
        if (!remove_player(entity_id)) {
            throw std::logic_error{"Absorbed player could not be removed from the Dots World"};
        }
    }
    for (const auto food_id : consumed_food_ids) {
        if (!remove_food(food_id)) {
            throw std::logic_error{"Consumed food could not be removed from the Dots World"};
        }
    }

    tick_ += mycore::time::TickDelta{1};
    last_tick_journal_ = TickJournal{.tick = tick_, .events = std::move(next_events)};
    journal = last_tick_journal_;
    return true;
}

const World::EntityLocation* World::find_location(EntityId entity_id) const noexcept {
    if (!entity_id.is_valid()) {
        return nullptr;
    }
    const auto location = entity_locations_.find(entity_id.value());
    if (location == entity_locations_.end()) {
        return nullptr;
    }
    return &location->second;
}

std::optional<std::size_t> World::find_index(EntityId entity_id) const noexcept {
    const auto* location = find_location(entity_id);
    if (location == nullptr || location->kind != EntityKind::Player) {
        return std::nullopt;
    }
    return location->index;
}

std::optional<std::size_t> World::find_food_index(EntityId entity_id) const noexcept {
    const auto* location = find_location(entity_id);
    if (location == nullptr || location->kind != EntityKind::Food) {
        return std::nullopt;
    }
    return location->index;
}

const OwnerCheckpoint* World::find_owner(PlayerOwnerId owner_id) const noexcept {
    const auto owner = std::lower_bound(owners_.begin(),
                                        owners_.end(),
                                        owner_id,
                                        [](const OwnerCheckpoint& state, PlayerOwnerId id) {
                                            return state.owner_id < id;
                                        });
    return owner != owners_.end() && owner->owner_id == owner_id ? &*owner : nullptr;
}

OwnerCheckpoint* World::find_owner(PlayerOwnerId owner_id) noexcept {
    return const_cast<OwnerCheckpoint*>(std::as_const(*this).find_owner(owner_id));
}

std::optional<EntityId> World::next_entity_id() const noexcept {
    if (next_entity_id_ == EntityId::kInvalidValue) {
        return std::nullopt;
    }
    return EntityId{next_entity_id_};
}

void World::reserve_player_capacity() {
    reserve_for_append(entity_ids_);
    reserve_for_append(owner_ids_);
    reserve_for_append(positions_);
    reserve_for_append(masses_);
    reserve_for_append(radii_);
    reserve_for_append(launch_velocities_);
    reserve_for_append(merge_eligible_ticks_);
    reserve_for_append(prediction_keys_);
}

void World::reserve_food_capacity() {
    reserve_for_append(food_entity_ids_);
    reserve_for_append(food_positions_);
}

} // namespace dots::simulation

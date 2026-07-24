#include "dots/simulation/world.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

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

} // namespace

std::optional<EntityId> World::spawn_player(PlayerOwnerId owner_id,
                                            mycore::math::Vector2 position) {
    const auto entity_id = next_entity_id();
    const auto radius = radius_for_mass(kInitialPlayerMass);
    const Circle bounds{.center = position, .radius = radius};
    if (!owner_id.is_valid() || !entity_id || !spatial_grid_.can_index(bounds)) {
        return std::nullopt;
    }

    reserve_player_capacity();
    if (!spatial_grid_.insert(*entity_id, bounds)) {
        return std::nullopt;
    }

    const auto index = entity_ids_.size();
    entity_ids_.push_back(*entity_id);
    owner_ids_.push_back(owner_id);
    positions_.push_back(position);
    movements_.emplace_back();
    last_input_ids_.emplace_back();
    masses_.push_back(kInitialPlayerMass);
    radii_.push_back(radius);
    entity_locations_.push_back(EntityLocation{.kind = EntityKind::Player, .index = index});
    ++next_entity_id_;
    return entity_id;
}

bool World::remove_player(EntityId entity_id) {
    const auto index = find_index(entity_id);
    if (!index) {
        return false;
    }

    if (!spatial_grid_.remove(entity_id)) {
        throw std::logic_error{"Player is missing from the Dots spatial grid"};
    }
    const auto last_index = entity_ids_.size() - 1;
    if (*index != last_index) {
        const auto moved_entity_id = entity_ids_[last_index];
        entity_ids_[*index] = entity_ids_[last_index];
        owner_ids_[*index] = owner_ids_[last_index];
        positions_[*index] = positions_[last_index];
        movements_[*index] = movements_[last_index];
        last_input_ids_[*index] = last_input_ids_[last_index];
        masses_[*index] = masses_[last_index];
        radii_[*index] = radii_[last_index];
        entity_locations_[moved_entity_id.value()] =
            EntityLocation{.kind = EntityKind::Player, .index = *index};
    }

    entity_ids_.pop_back();
    owner_ids_.pop_back();
    positions_.pop_back();
    movements_.pop_back();
    last_input_ids_.pop_back();
    masses_.pop_back();
    radii_.pop_back();
    entity_locations_[entity_id.value()].reset();
    return true;
}

std::optional<EntityId> World::spawn_food(mycore::math::Vector2 position) {
    const auto entity_id = next_entity_id();
    const Circle bounds{.center = position, .radius = radius_for_mass(kFoodMass)};
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
    entity_locations_.push_back(EntityLocation{.kind = EntityKind::Food, .index = index});
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
    entity_locations_[entity_id.value()].reset();
    return true;
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
    return kFoodMass;
}

std::optional<float> World::radius(EntityId entity_id) const noexcept {
    const auto* location = find_location(entity_id);
    if (location == nullptr) {
        return std::nullopt;
    }
    if (location->kind == EntityKind::Player) {
        return radii_[location->index];
    }
    return radius_for_mass(kFoodMass);
}

bool World::has_available_entity_id() const noexcept {
    return next_entity_id().has_value();
}

InitialPlayerSpawnStatus
World::classify_initial_player_spawn(mycore::math::Vector2 position) const {
    const Circle candidate_circle{.center = position,
                                  .radius = radius_for_mass(kInitialPlayerMass)};
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

std::span<const PlayerAbsorbed> World::last_step_events() const noexcept {
    return last_step_events_;
}

bool World::apply_input(const InputCommand& command) {
    if (!command.id.is_valid() || !command.entity_id.is_valid() ||
        !std::isfinite(command.movement.x) || !std::isfinite(command.movement.y)) {
        return false;
    }

    const auto index = find_index(command.entity_id);
    if (!index) {
        return false;
    }

    const auto last_input_id = last_input_ids_[*index];
    if (last_input_id.is_valid() && command.id <= last_input_id) {
        return false;
    }

    movements_[*index] = normalized_player_movement(command.movement);
    last_input_ids_[*index] = command.id;
    return true;
}

bool World::stop_player(EntityId entity_id) noexcept {
    const auto index = find_index(entity_id);
    if (!index) {
        return false;
    }
    movements_[*index] = {};
    return true;
}

bool World::step() {
    std::vector<mycore::math::Vector2> next_positions;
    next_positions.reserve(positions_.size());
    for (std::size_t index = 0; index < positions_.size(); ++index) {
        const auto next_position = advance_player_position(positions_[index], movements_[index]);
        if (!spatial_grid_.can_index({.center = next_position, .radius = radii_[index]})) {
            return false;
        }
        next_positions.push_back(next_position);
    }

    SpatialGrid collision_grid{kSpatialGridCellSize};
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
    std::vector<PlayerAbsorbed> next_events;
    next_events.reserve(absorption_candidates.size());
    auto completed_tick = tick_;
    completed_tick += mycore::time::TickDelta{1};
    for (const auto& candidate : absorption_candidates) {
        if (!live_players[candidate.absorber_index] || !live_players[candidate.victim_index]) {
            continue;
        }
        live_players[candidate.victim_index] = false;
        mass_gains[candidate.absorber_index] += masses_[candidate.victim_index];
        next_events.push_back({
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
                                     .radius = radius_for_mass(kFoodMass)};
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
        mass_gains[consumption.player_index] += kFoodMass;
        consumed_food_ids.push_back(consumption.food_id);
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
    last_step_events_ = std::move(next_events);
    return true;
}

const World::EntityLocation* World::find_location(EntityId entity_id) const noexcept {
    if (!entity_id.is_valid()) {
        return nullptr;
    }
    const auto index = static_cast<std::size_t>(entity_id.value());
    if (index >= entity_locations_.size()) {
        return nullptr;
    }
    const auto& location = entity_locations_[index];
    if (!location) {
        return nullptr;
    }
    return &*location;
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
    reserve_for_append(movements_);
    reserve_for_append(last_input_ids_);
    reserve_for_append(masses_);
    reserve_for_append(radii_);
    reserve_for_append(entity_locations_);
}

void World::reserve_food_capacity() {
    reserve_for_append(food_entity_ids_);
    reserve_for_append(food_positions_);
    reserve_for_append(entity_locations_);
}

} // namespace dots::simulation

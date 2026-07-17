#include "dots/simulation/world.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace dots::simulation {
namespace {

struct FoodConsumption {
    EntityId food_id;
    EntityId player_id;
};

template <typename T> void reserve_for_append(std::vector<T>& values) {
    if (values.size() < values.capacity()) {
        return;
    }
    const auto next_capacity = values.capacity() == 0 ? std::size_t{1} : values.capacity() * 2;
    values.reserve(next_capacity);
}

} // namespace

std::optional<EntityId> World::spawn_player(mycore::math::Vector2 position) {
    const auto entity_id = next_entity_id();
    const auto radius = radius_for_mass(kInitialPlayerMass);
    const Circle bounds{.center = position, .radius = radius};
    if (!entity_id || !spatial_grid_.can_index(bounds)) {
        return std::nullopt;
    }

    reserve_player_capacity();
    if (!spatial_grid_.insert(*entity_id, bounds)) {
        return std::nullopt;
    }

    const auto index = entity_ids_.size();
    entity_ids_.push_back(*entity_id);
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
        positions_[*index] = positions_[last_index];
        movements_[*index] = movements_[last_index];
        last_input_ids_[*index] = last_input_ids_[last_index];
        masses_[*index] = masses_[last_index];
        radii_[*index] = radii_[last_index];
        entity_locations_[moved_entity_id.value()] =
            EntityLocation{.kind = EntityKind::Player, .index = *index};
    }

    entity_ids_.pop_back();
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

std::span<const EntityId> World::player_ids() const noexcept {
    return entity_ids_;
}

std::span<const EntityId> World::food_ids() const noexcept {
    return food_entity_ids_;
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

mycore::time::Tick World::tick() const noexcept {
    return tick_;
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

    movements_[*index] = mycore::math::normalized_or_zero(command.movement);
    last_input_ids_[*index] = command.id;
    return true;
}

bool World::step() {
    constexpr auto kDistancePerTick = kPlayerSpeedUnitsPerSecond / static_cast<float>(kTickRateHz);
    std::vector<mycore::math::Vector2> next_positions;
    next_positions.reserve(positions_.size());
    for (std::size_t index = 0; index < positions_.size(); ++index) {
        const auto next_position = positions_[index] + movements_[index] * kDistancePerTick;
        if (!spatial_grid_.can_index({.center = next_position, .radius = radii_[index]})) {
            return false;
        }
        next_positions.push_back(next_position);
    }

    for (std::size_t index = 0; index < positions_.size(); ++index) {
        if (!spatial_grid_.update(entity_ids_[index],
                                  {.center = next_positions[index], .radius = radii_[index]})) {
            throw std::logic_error{"Player could not be updated in the Dots spatial grid"};
        }
        positions_[index] = next_positions[index];
    }

    resolve_food_collisions();
    tick_ += mycore::time::TickDelta{1};
    return true;
}

const World::EntityLocation* World::find_location(EntityId entity_id) const noexcept {
    if (!entity_id.is_valid()) {
        return nullptr;
    }
    const auto index = static_cast<std::size_t>(entity_id.value());
    if (index >= entity_locations_.size() || !entity_locations_[index]) {
        return nullptr;
    }
    return &*entity_locations_[index];
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

void World::resolve_food_collisions() {
    if (food_entity_ids_.empty()) {
        return;
    }

    // Gather every overlap before changing mass so growth only affects the following tick.
    std::vector<FoodConsumption> consumptions;
    for (std::size_t player_index = 0; player_index < entity_ids_.size(); ++player_index) {
        const Circle player_circle{.center = positions_[player_index],
                                   .radius = radii_[player_index]};
        const auto candidates = spatial_grid_.query(player_circle);
        for (const auto candidate : candidates) {
            const auto food_index = find_food_index(candidate);
            if (!food_index) {
                continue;
            }

            const Circle food_circle{.center = food_positions_[*food_index],
                                     .radius = radius_for_mass(kFoodMass)};
            if (circles_overlap(player_circle, food_circle)) {
                consumptions.push_back(
                    {.food_id = candidate, .player_id = entity_ids_[player_index]});
            }
        }
    }

    if (consumptions.empty()) {
        return;
    }

    std::sort(consumptions.begin(),
              consumptions.end(),
              [](const FoodConsumption& lhs, const FoodConsumption& rhs) {
                  if (lhs.food_id != rhs.food_id) {
                      return lhs.food_id < rhs.food_id;
                  }
                  return lhs.player_id < rhs.player_id;
              });

    std::vector<float> mass_gains(entity_ids_.size());
    auto last_food_id = EntityId::invalid();
    for (const auto& consumption : consumptions) {
        if (consumption.food_id == last_food_id) {
            continue;
        }
        last_food_id = consumption.food_id;
        const auto player_index = find_index(consumption.player_id);
        if (!player_index) {
            throw std::logic_error{"Food consumption references an unknown player"};
        }
        mass_gains[*player_index] += kFoodMass;
    }

    auto next_radii = radii_;
    for (std::size_t player_index = 0; player_index < entity_ids_.size(); ++player_index) {
        if (mass_gains[player_index] == 0.0F) {
            continue;
        }
        const auto next_radius = radius_for_mass(masses_[player_index] + mass_gains[player_index]);
        if (!spatial_grid_.can_index({.center = positions_[player_index], .radius = next_radius})) {
            throw std::overflow_error{"Player growth exceeds the Dots spatial-grid range"};
        }
        next_radii[player_index] = next_radius;
    }

    for (std::size_t player_index = 0; player_index < entity_ids_.size(); ++player_index) {
        if (mass_gains[player_index] == 0.0F) {
            continue;
        }
        masses_[player_index] += mass_gains[player_index];
        radii_[player_index] = next_radii[player_index];
    }

    last_food_id = EntityId::invalid();
    for (const auto& consumption : consumptions) {
        if (consumption.food_id == last_food_id) {
            continue;
        }
        last_food_id = consumption.food_id;
        if (!remove_food(consumption.food_id)) {
            throw std::logic_error{"Food consumption references unknown food"};
        }
    }

    for (std::size_t player_index = 0; player_index < entity_ids_.size(); ++player_index) {
        if (mass_gains[player_index] == 0.0F) {
            continue;
        }
        if (!spatial_grid_.update(
                entity_ids_[player_index],
                {.center = positions_[player_index], .radius = radii_[player_index]})) {
            throw std::logic_error{"Player growth could not update the Dots spatial grid"};
        }
    }
}

} // namespace dots::simulation

#include "dots/simulation/world.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>

namespace dots::simulation {

EntityId World::spawn_player(mycore::math::Vector2 position) {
    if (next_entity_id_ == EntityId::kInvalidValue) {
        throw std::overflow_error{"Dots entity ID space exhausted"};
    }

    const EntityId entity_id{next_entity_id_++};
    entity_ids_.push_back(entity_id);
    positions_.push_back(position);
    movements_.emplace_back();
    last_input_ids_.emplace_back();
    return entity_id;
}

bool World::remove_player(EntityId entity_id) {
    const auto index = find_index(entity_id);
    if (!index) {
        return false;
    }

    const auto last_index = entity_ids_.size() - 1;
    if (*index != last_index) {
        entity_ids_[*index] = entity_ids_[last_index];
        positions_[*index] = positions_[last_index];
        movements_[*index] = movements_[last_index];
        last_input_ids_[*index] = last_input_ids_[last_index];
    }

    entity_ids_.pop_back();
    positions_.pop_back();
    movements_.pop_back();
    last_input_ids_.pop_back();
    return true;
}

bool World::contains(EntityId entity_id) const noexcept {
    return find_index(entity_id).has_value();
}

std::size_t World::player_count() const noexcept {
    return entity_ids_.size();
}

std::optional<mycore::math::Vector2> World::position(EntityId entity_id) const noexcept {
    const auto index = find_index(entity_id);
    if (!index) {
        return std::nullopt;
    }
    return positions_[*index];
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

void World::step() noexcept {
    constexpr auto kDistancePerTick = kPlayerSpeedUnitsPerSecond / static_cast<float>(kTickRateHz);
    for (std::size_t index = 0; index < positions_.size(); ++index) {
        positions_[index] += movements_[index] * kDistancePerTick;
    }
    tick_ += mycore::time::TickDelta{1};
}

std::optional<std::size_t> World::find_index(EntityId entity_id) const noexcept {
    const auto iterator = std::find(entity_ids_.begin(), entity_ids_.end(), entity_id);
    if (iterator == entity_ids_.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(entity_ids_.begin(), iterator));
}

} // namespace dots::simulation

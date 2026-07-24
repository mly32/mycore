#pragma once

#include "dots/simulation/ids.hpp"
#include "mycore/math/vector2.hpp"

#include <cstdint>
#include <variant>

namespace dots::simulation {

class World;

enum class SafePlayerSpawnError : std::uint8_t {
    EntityIdExhausted,
    NoSafePosition,
};

using SafePlayerSpawnResult = std::variant<EntityId, SafePlayerSpawnError>;

[[nodiscard]] bool spawn_default_food_field(World& world);
[[nodiscard]] mycore::math::Vector2 initial_player_spawn_candidate(std::uint64_t ordinal) noexcept;
[[nodiscard]] SafePlayerSpawnResult spawn_player_safely(World& world, PlayerOwnerId owner_id);

} // namespace dots::simulation

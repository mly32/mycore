#pragma once

#include "dots/simulation/ids.hpp"

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
[[nodiscard]] SafePlayerSpawnResult spawn_player_safely(World& world, PlayerOwnerId owner_id);

} // namespace dots::simulation

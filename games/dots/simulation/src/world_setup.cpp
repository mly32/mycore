#include "dots/simulation/world_setup.hpp"

#include "dots/simulation/world.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace dots::simulation {

bool spawn_default_food_field(World& world) {
    constexpr float kSpacing = 8.0F;
    for (int row = -6; row <= 6; ++row) {
        for (int column = -10; column <= 10; ++column) {
            if (row == 0 && column == 0) {
                continue;
            }
            if (!world.spawn_food(
                    {static_cast<float>(column) * kSpacing, static_cast<float>(row) * kSpacing})) {
                return false;
            }
        }
    }
    return true;
}

SafePlayerSpawnResult spawn_player_safely(World& world, PlayerOwnerId owner_id) {
    if (!world.has_available_entity_id()) {
        return SafePlayerSpawnError::EntityIdExhausted;
    }

    constexpr double kSpawnSpacing = 12.0;
    std::int64_t lattice_x{};
    std::int64_t lattice_y{};
    std::int64_t direction_x{};
    std::int64_t direction_y{-1};

    while (true) {
        const auto world_x = static_cast<double>(lattice_x) * kSpawnSpacing;
        const auto world_y = static_cast<double>(lattice_y) * kSpawnSpacing;
        if (std::abs(world_x) > static_cast<double>(std::numeric_limits<float>::max()) ||
            std::abs(world_y) > static_cast<double>(std::numeric_limits<float>::max())) {
            return SafePlayerSpawnError::NoSafePosition;
        }
        const mycore::math::Vector2 candidate{static_cast<float>(world_x),
                                              static_cast<float>(world_y)};
        if (!world.can_index_initial_player(candidate)) {
            return SafePlayerSpawnError::NoSafePosition;
        }

        if (world.is_initial_player_spawn_clear(candidate)) {
            const auto player = world.spawn_player(owner_id, candidate);
            return player ? SafePlayerSpawnResult{*player}
                          : SafePlayerSpawnResult{SafePlayerSpawnError::EntityIdExhausted};
        }

        if (lattice_x == lattice_y || (lattice_x < 0 && lattice_x == -lattice_y) ||
            (lattice_x > 0 && lattice_x == 1 - lattice_y)) {
            const auto next_direction_x = -direction_y;
            direction_y = direction_x;
            direction_x = next_direction_x;
        }
        lattice_x += direction_x;
        lattice_y += direction_y;
    }
}

} // namespace dots::simulation

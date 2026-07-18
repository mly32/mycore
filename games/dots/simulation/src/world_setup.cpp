#include "dots/simulation/world_setup.hpp"

#include "dots/simulation/world.hpp"

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

} // namespace dots::simulation

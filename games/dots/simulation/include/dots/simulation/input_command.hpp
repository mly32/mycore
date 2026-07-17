#pragma once

#include "dots/simulation/ids.hpp"
#include "mycore/math/vector2.hpp"

namespace dots::simulation {

// A player's desired movement direction at a specific input sequence.
struct InputCommand {
    InputCommandId id;
    EntityId entity_id;
    mycore::math::Vector2 movement;
};

} // namespace dots::simulation

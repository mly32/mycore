#include "dots/simulation/tick.hpp"

#include <concepts>
#include <type_traits>

namespace dots::simulation {

SimulationEventKey simulation_event_key(const SimulationEvent& event) {
    return std::visit(
        [](const auto& value) -> SimulationEventKey {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<Event, FoodConsumed>) {
                return FoodConsumedKey{.food_entity_id = value.food_entity_id};
            } else {
                static_assert(std::same_as<Event, PlayerAbsorbed>);
                return PlayerAbsorbedKey{.victim_entity_id = value.victim_entity_id};
            }
        },
        event);
}

} // namespace dots::simulation

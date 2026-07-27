#include "dots/simulation/tick.hpp"

#include <algorithm>
#include <concepts>
#include <type_traits>

namespace dots::simulation {

SimulationEventKey simulation_event_key(const SimulationEvent& event) {
    return std::visit(
        [](const auto& value) -> SimulationEventKey {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<Event, FoodConsumed>) {
                return FoodConsumedKey{.food_entity_id = value.food_entity_id};
            } else if constexpr (std::same_as<Event, PlayerAbsorbed>) {
                return PlayerAbsorbedKey{.victim_entity_id = value.victim_entity_id};
            } else if constexpr (std::same_as<Event, PlayerSplit>) {
                return PlayerSplitKey{
                    .owner_id = value.owner_id,
                    .input_id = value.input_id,
                    .child_ordinal = value.child_ordinal,
                };
            } else {
                static_assert(std::same_as<Event, PiecesMerged>);
                return PiecesMergedKey{
                    .first_entity_id = std::min(value.survivor_entity_id, value.consumed_entity_id),
                    .second_entity_id =
                        std::max(value.survivor_entity_id, value.consumed_entity_id),
                };
            }
        },
        event);
}

} // namespace dots::simulation

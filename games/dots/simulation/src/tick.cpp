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

SimulationEventParticipants simulation_event_participants(const SimulationEvent& event) noexcept {
    return std::visit(
        [](const auto& value) {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<Event, PlayerAbsorbed>) {
                if (value.absorber_owner_id == value.victim_owner_id) {
                    return SimulationEventParticipants{
                        .owner_ids = {value.absorber_owner_id, {}},
                        .count = 1,
                    };
                }
                const auto first = std::min(value.absorber_owner_id, value.victim_owner_id);
                const auto second = std::max(value.absorber_owner_id, value.victim_owner_id);
                return SimulationEventParticipants{
                    .owner_ids = {first, second},
                    .count = 2,
                };
            } else if constexpr (std::same_as<Event, FoodConsumed>) {
                return SimulationEventParticipants{
                    .owner_ids = {value.consumer_owner_id, {}},
                    .count = 1,
                };
            } else {
                static_assert(std::same_as<Event, PlayerSplit> ||
                              std::same_as<Event, PiecesMerged>);
                return SimulationEventParticipants{
                    .owner_ids = {value.owner_id, {}},
                    .count = 1,
                };
            }
        },
        event);
}

bool simulation_event_involves_owner(const SimulationEvent& event,
                                     PlayerOwnerId owner_id) noexcept {
    const auto participants = simulation_event_participants(event);
    return std::binary_search(participants.owners().begin(), participants.owners().end(), owner_id);
}

} // namespace dots::simulation

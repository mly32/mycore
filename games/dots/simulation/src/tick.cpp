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
    if (const auto* absorbed = std::get_if<PlayerAbsorbed>(&event)) {
        if (absorbed->absorber_owner_id == absorbed->victim_owner_id) {
            return {
                .owner_ids = {absorbed->absorber_owner_id, {}},
                .count = 1,
            };
        }
        const auto first = std::min(absorbed->absorber_owner_id, absorbed->victim_owner_id);
        const auto second = std::max(absorbed->absorber_owner_id, absorbed->victim_owner_id);
        return {
            .owner_ids = {first, second},
            .count = 2,
        };
    }
    if (const auto* consumed = std::get_if<FoodConsumed>(&event)) {
        return {
            .owner_ids = {consumed->consumer_owner_id, {}},
            .count = 1,
        };
    }
    if (const auto* split = std::get_if<PlayerSplit>(&event)) {
        return {
            .owner_ids = {split->owner_id, {}},
            .count = 1,
        };
    }
    if (const auto* merged = std::get_if<PiecesMerged>(&event)) {
        return {
            .owner_ids = {merged->owner_id, {}},
            .count = 1,
        };
    }
    return {};
}

bool simulation_event_involves_owner(const SimulationEvent& event,
                                     PlayerOwnerId owner_id) noexcept {
    const auto participants = simulation_event_participants(event);
    return std::binary_search(participants.owners().begin(), participants.owners().end(), owner_id);
}

} // namespace dots::simulation

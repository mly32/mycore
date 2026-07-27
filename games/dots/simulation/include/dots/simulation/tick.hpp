#pragma once

#include "dots/simulation/world_state.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/time/time.hpp"

#include <compare>
#include <cstdint>
#include <variant>
#include <vector>

namespace dots::simulation {

enum class TickCommandType : std::uint8_t {
    ApplyInput,
    StopMovement,
    AssumeMovement,
};

// A tick accepts at most one command for each owner. ApplyInput installs a newer sampled movement
// and command identity and may carry one split edge action. StopMovement is an explicit
// deterministic cause with an invalid input ID and zero movement. AssumeMovement installs a
// retained remote level assumption with an invalid input ID without claiming that the
// authoritative owner consumed a new command.
struct TickCommand {
    TickCommandType type{TickCommandType::ApplyInput};
    InputCommandId input_id;
    PlayerOwnerId owner_id;
    mycore::math::Vector2 movement;
    bool split_requested{};

    bool operator==(const TickCommand&) const = default;
};

struct TickMechanics {
    bool player_absorption{true};
    bool food_consumption{true};
    bool split_merge{true};

    bool operator==(const TickMechanics&) const = default;
};

struct FoodConsumed {
    mycore::time::Tick tick;
    EntityId food_entity_id;
    EntityId consumer_entity_id;
    PlayerOwnerId consumer_owner_id;
    float transferred_mass{};

    bool operator==(const FoodConsumed&) const = default;
};

struct PlayerAbsorbed {
    mycore::time::Tick tick;
    EntityId absorber_entity_id;
    EntityId victim_entity_id;
    PlayerOwnerId absorber_owner_id;
    PlayerOwnerId victim_owner_id;
    float transferred_mass{};

    bool operator==(const PlayerAbsorbed&) const = default;
};

struct PlayerSplit {
    mycore::time::Tick tick;
    PlayerOwnerId owner_id;
    InputCommandId input_id;
    std::uint16_t child_ordinal{};
    EntityId parent_entity_id;
    EntityId child_entity_id;
    float parent_mass{};
    float child_mass{};

    bool operator==(const PlayerSplit&) const = default;
};

struct PiecesMerged {
    mycore::time::Tick tick;
    PlayerOwnerId owner_id;
    EntityId survivor_entity_id;
    EntityId consumed_entity_id;
    float combined_mass{};

    bool operator==(const PiecesMerged&) const = default;
};

using SimulationEvent = std::variant<FoodConsumed, PlayerAbsorbed, PlayerSplit, PiecesMerged>;

struct FoodConsumedKey {
    EntityId food_entity_id;

    auto operator<=>(const FoodConsumedKey&) const = default;
};

struct PlayerAbsorbedKey {
    EntityId victim_entity_id;

    auto operator<=>(const PlayerAbsorbedKey&) const = default;
};

struct PlayerSplitKey {
    PlayerOwnerId owner_id;
    InputCommandId input_id;
    std::uint16_t child_ordinal{};

    auto operator<=>(const PlayerSplitKey&) const = default;
};

struct PiecesMergedKey {
    EntityId first_entity_id;
    EntityId second_entity_id;

    auto operator<=>(const PiecesMergedKey&) const = default;
};

using SimulationEventKey =
    std::variant<FoodConsumedKey, PlayerAbsorbedKey, PlayerSplitKey, PiecesMergedKey>;

[[nodiscard]] SimulationEventKey simulation_event_key(const SimulationEvent& event);

// Journals are deterministic simulation output. Replay regenerates them; they are never restored
// as checkpoint state or allowed to invoke presentation side effects from inside the World.
struct TickJournal {
    mycore::time::Tick tick;
    std::vector<SimulationEvent> events;

    bool operator==(const TickJournal&) const = default;
};

enum class TickError : std::uint8_t {
    InvalidCommand,
    DuplicateOwnerCommand,
    SimulationRejected,
};

using TickResult = std::variant<TickJournal, TickError>;

} // namespace dots::simulation

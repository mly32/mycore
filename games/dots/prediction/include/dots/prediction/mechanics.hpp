#pragma once

#include "dots/prediction/types.hpp"

#include <array>

namespace dots::prediction {

enum class MechanicEventKind : std::uint8_t {
    FoodConsumed,
    PlayerAbsorbed,
    PlayerSplit,
    PiecesMerged,
};

using MechanicEventMask = std::uint32_t;

[[nodiscard]] constexpr MechanicEventMask
mechanic_event_bit(MechanicEventKind event_kind) noexcept {
    return MechanicEventMask{1} << static_cast<std::uint8_t>(event_kind);
}

[[nodiscard]] constexpr bool includes_mechanic_event(MechanicEventMask events,
                                                     MechanicEventKind event_kind) noexcept {
    return (events & mechanic_event_bit(event_kind)) != 0U;
}

struct MechanicContract {
    PredictionMechanic mechanic{PredictionMechanic::Movement};
    MechanicMask dependencies{};
    StateDomainMask reads{};
    StateDomainMask writes{};
    StateDomainMask presentation_reads{};
    MechanicEventMask events{};
    bool expands_ownership{};
    bool expands_spatial_interactions{};
    bool implemented{};

    bool operator==(const MechanicContract&) const = default;
};

inline constexpr std::array kMechanicContracts{
    MechanicContract{
        .mechanic = PredictionMechanic::Movement,
        .dependencies = 0,
        .reads = state_domain_bit(StateDomain::WorldRules) |
                 state_domain_bit(StateDomain::WorldTick) |
                 state_domain_bit(StateDomain::OwnerCommands) |
                 state_domain_bit(StateDomain::OwnerTopology) |
                 state_domain_bit(StateDomain::PlayerKinematics) |
                 state_domain_bit(StateDomain::PlayerMass),
        .writes = state_domain_bit(StateDomain::WorldTick) |
                  state_domain_bit(StateDomain::OwnerCommands) |
                  state_domain_bit(StateDomain::PlayerKinematics),
        .presentation_reads = state_domain_bit(StateDomain::PlayerKinematics),
        .events = 0,
        .expands_ownership = true,
        .expands_spatial_interactions = false,
        .implemented = true,
    },
    MechanicContract{
        .mechanic = PredictionMechanic::FoodConsumption,
        .dependencies = mechanic_bit(PredictionMechanic::Movement),
        .reads = state_domain_bit(StateDomain::WorldRules) |
                 state_domain_bit(StateDomain::PlayerKinematics) |
                 state_domain_bit(StateDomain::PlayerMass) |
                 state_domain_bit(StateDomain::FoodState),
        .writes = state_domain_bit(StateDomain::PlayerMass) |
                  state_domain_bit(StateDomain::FoodState) |
                  state_domain_bit(StateDomain::EventJournal),
        .presentation_reads =
            state_domain_bit(StateDomain::PlayerMass) | state_domain_bit(StateDomain::FoodState),
        .events = mechanic_event_bit(MechanicEventKind::FoodConsumed),
        .expands_ownership = false,
        .expands_spatial_interactions = true,
        .implemented = true,
    },
    MechanicContract{
        .mechanic = PredictionMechanic::PlayerAbsorption,
        .dependencies = mechanic_bit(PredictionMechanic::Movement),
        .reads = state_domain_bit(StateDomain::OwnerTopology) |
                 state_domain_bit(StateDomain::PlayerKinematics) |
                 state_domain_bit(StateDomain::PlayerMass),
        .writes = state_domain_bit(StateDomain::OwnerTopology) |
                  state_domain_bit(StateDomain::PlayerMass) |
                  state_domain_bit(StateDomain::EventJournal),
        .presentation_reads = state_domain_bit(StateDomain::OwnerTopology) |
                              state_domain_bit(StateDomain::PlayerKinematics) |
                              state_domain_bit(StateDomain::PlayerMass),
        .events = mechanic_event_bit(MechanicEventKind::PlayerAbsorbed),
        .expands_ownership = true,
        .expands_spatial_interactions = true,
        .implemented = true,
    },
    MechanicContract{
        .mechanic = PredictionMechanic::SplitMerge,
        .dependencies = mechanic_bit(PredictionMechanic::Movement),
        .reads = state_domain_bit(StateDomain::WorldRules) |
                 state_domain_bit(StateDomain::WorldTick) |
                 state_domain_bit(StateDomain::EntityAllocator) |
                 state_domain_bit(StateDomain::OwnerCommands) |
                 state_domain_bit(StateDomain::OwnerTopology) |
                 state_domain_bit(StateDomain::PlayerKinematics) |
                 state_domain_bit(StateDomain::PlayerMass) |
                 state_domain_bit(StateDomain::PredictedIdentity),
        .writes = state_domain_bit(StateDomain::EntityAllocator) |
                  state_domain_bit(StateDomain::OwnerCommands) |
                  state_domain_bit(StateDomain::OwnerTopology) |
                  state_domain_bit(StateDomain::PlayerKinematics) |
                  state_domain_bit(StateDomain::PlayerMass) |
                  state_domain_bit(StateDomain::PredictedIdentity) |
                  state_domain_bit(StateDomain::EventJournal),
        .presentation_reads = state_domain_bit(StateDomain::OwnerTopology) |
                              state_domain_bit(StateDomain::PlayerKinematics) |
                              state_domain_bit(StateDomain::PlayerMass) |
                              state_domain_bit(StateDomain::PredictedIdentity),
        .events = mechanic_event_bit(MechanicEventKind::PlayerSplit) |
                  mechanic_event_bit(MechanicEventKind::PiecesMerged),
        .expands_ownership = true,
        .expands_spatial_interactions = true,
        .implemented = true,
    },
};

static_assert(kMechanicContracts.size() ==
              static_cast<std::size_t>(PredictionMechanic::SplitMerge) + 1U);

[[nodiscard]] constexpr const MechanicContract&
mechanic_contract(PredictionMechanic mechanic) noexcept {
    return kMechanicContracts[static_cast<std::size_t>(mechanic)];
}

} // namespace dots::prediction

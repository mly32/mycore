#pragma once

#include "dots/simulation/tick.hpp"
#include "dots/simulation/world_state.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/rollback/types.hpp"
#include "mycore/time/time.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace dots::prediction {

enum class PredictionProfile : std::uint8_t {
    InteractionClosure,
    FullReplicated,
    OwnedGameplay,
};

enum class PredictionFallbackReason : std::uint8_t {
    None,
    IncompleteClosure,
};

enum class PredictionMechanic : std::uint8_t {
    Movement,
    FoodConsumption,
    PlayerAbsorption,
    SplitMerge,
};

using MechanicMask = std::uint32_t;

[[nodiscard]] constexpr MechanicMask mechanic_bit(PredictionMechanic mechanic) noexcept {
    return MechanicMask{1} << static_cast<std::uint8_t>(mechanic);
}

[[nodiscard]] constexpr bool includes_mechanic(MechanicMask mechanics,
                                               PredictionMechanic mechanic) noexcept {
    return (mechanics & mechanic_bit(mechanic)) != 0U;
}

inline constexpr MechanicMask kCurrentPredictionMechanics =
    mechanic_bit(PredictionMechanic::Movement) | mechanic_bit(PredictionMechanic::FoodConsumption) |
    mechanic_bit(PredictionMechanic::PlayerAbsorption) |
    mechanic_bit(PredictionMechanic::SplitMerge);

enum class StateDomain : std::uint8_t {
    WorldRules,
    WorldTick,
    EntityAllocator,
    OwnerCommands,
    OwnerTopology,
    PlayerKinematics,
    PlayerMass,
    FoodState,
    EventJournal,
    PredictedIdentity,
};

using StateDomainMask = std::uint32_t;

[[nodiscard]] constexpr StateDomainMask state_domain_bit(StateDomain domain) noexcept {
    return StateDomainMask{1} << static_cast<std::uint8_t>(domain);
}

[[nodiscard]] constexpr bool includes_state_domain(StateDomainMask domains,
                                                   StateDomain domain) noexcept {
    return (domains & state_domain_bit(domain)) != 0U;
}

inline constexpr StateDomainMask kAllStateDomains =
    state_domain_bit(StateDomain::WorldRules) | state_domain_bit(StateDomain::WorldTick) |
    state_domain_bit(StateDomain::EntityAllocator) | state_domain_bit(StateDomain::OwnerCommands) |
    state_domain_bit(StateDomain::OwnerTopology) | state_domain_bit(StateDomain::PlayerKinematics) |
    state_domain_bit(StateDomain::PlayerMass) | state_domain_bit(StateDomain::FoodState) |
    state_domain_bit(StateDomain::EventJournal) | state_domain_bit(StateDomain::PredictedIdentity);

enum class CausalChannel : std::uint8_t {
    RemoteMovement,
    ScheduledExternalFacts,
};

using CausalChannelMask = std::uint32_t;

[[nodiscard]] constexpr CausalChannelMask causal_channel_bit(CausalChannel channel) noexcept {
    return CausalChannelMask{1} << static_cast<std::uint8_t>(channel);
}

[[nodiscard]] constexpr bool includes_causal_channel(CausalChannelMask channels,
                                                     CausalChannel channel) noexcept {
    return (channels & causal_channel_bit(channel)) != 0U;
}

inline constexpr CausalChannelMask kAllCausalChannels =
    causal_channel_bit(CausalChannel::RemoteMovement) |
    causal_channel_bit(CausalChannel::ScheduledExternalFacts);

struct AuthorityCoverage {
    StateDomainMask available_domains{kAllStateDomains};
    CausalChannelMask available_causal_channels{kAllCausalChannels};
    bool complete_entity_set{true};
    bool complete_spatial_neighborhood{true};

    bool operator==(const AuthorityCoverage&) const = default;
};

struct PredictionRequest {
    PredictionProfile profile{PredictionProfile::InteractionClosure};
    MechanicMask mechanics{kCurrentPredictionMechanics};
    std::vector<simulation::PlayerOwnerId> owned_owner_ids;
    std::vector<simulation::PlayerOwnerId> subscribed_event_owner_ids;
    mycore::time::TickDelta replay_horizon{64};
    mycore::rollback::ScopeEpoch scope_epoch;
    AuthorityCoverage coverage;

    bool operator==(const PredictionRequest&) const = default;
};

// Scope membership is the maximum authoritative island admitted by this epoch. Live checkpoint
// state may contain a subset after consumption/absorption and locally predicted owned children
// identified by unique PredictionKeys, because those entity IDs do not exist at scope-build time.
struct PredictionScope {
    PredictionProfile requested_profile{PredictionProfile::InteractionClosure};
    PredictionProfile active_profile{PredictionProfile::InteractionClosure};
    PredictionFallbackReason fallback_reason{PredictionFallbackReason::None};
    MechanicMask requested_mechanics{kCurrentPredictionMechanics};
    MechanicMask mechanics{kCurrentPredictionMechanics};
    StateDomainMask required_domains{};
    CausalChannelMask required_causal_channels{};
    std::vector<simulation::PlayerOwnerId> owned_owner_ids;
    std::vector<simulation::PlayerOwnerId> subscribed_event_owner_ids;
    std::vector<simulation::PlayerOwnerId> owner_ids;
    std::vector<simulation::EntityId> player_ids;
    std::vector<simulation::EntityId> food_ids;
    simulation::WorldRules rules;
    mycore::time::TickDelta replay_horizon;
    mycore::rollback::ScopeEpoch scope_epoch;

    bool operator==(const PredictionScope&) const = default;
};

struct HeldMovementAssumption {
    simulation::PlayerOwnerId owner_id;
    mycore::time::Tick source_tick;
    mycore::math::Vector2 movement;

    bool operator==(const HeldMovementAssumption&) const = default;
};

// Device and transport state is sampled before this immutable value enters rollback history.
// Remote assumptions are explicit causes; simulation events are regenerated outputs.
struct TickStimulus {
    std::vector<simulation::TickCommand> commands;
    std::vector<HeldMovementAssumption> remote_movement_assumptions;

    bool operator==(const TickStimulus&) const = default;
};

struct StateDigest {
    std::uint64_t value{};

    auto operator<=>(const StateDigest&) const = default;
};

struct OwnerDifference {
    simulation::PlayerOwnerId owner_id;
    std::optional<simulation::OwnerCheckpoint> previous;
    std::optional<simulation::OwnerCheckpoint> current;

    bool operator==(const OwnerDifference&) const = default;
};

struct PlayerDifference {
    simulation::EntityId entity_id;
    std::optional<simulation::PlayerCheckpoint> previous;
    std::optional<simulation::PlayerCheckpoint> current;

    bool operator==(const PlayerDifference&) const = default;
};

struct FoodDifference {
    simulation::EntityId entity_id;
    std::optional<simulation::FoodCheckpoint> previous;
    std::optional<simulation::FoodCheckpoint> current;

    bool operator==(const FoodDifference&) const = default;
};

struct StateDifference {
    mycore::time::Tick previous_tick;
    mycore::time::Tick current_tick;
    bool rules_changed{};
    bool allocator_changed{};
    bool structural_change{};
    float maximum_position_delta{};
    float maximum_mass_delta{};
    std::vector<OwnerDifference> owners;
    std::vector<PlayerDifference> players;
    std::vector<FoodDifference> food;

    bool operator==(const StateDifference&) const = default;
};

enum class ScopeBuildError : std::uint8_t {
    InvalidRequest,
    InvalidCheckpoint,
    MissingOwnedOwner,
    IncompleteOwnedState,
    UnsupportedMechanic,
};

enum class PredictionErrorCode : std::uint8_t {
    InvalidScope,
    IncompatibleRules,
    CheckpointOutsideScope,
    CheckpointRestoreFailed,
    InvalidStimulus,
    TickFailed,
};

struct PredictionError {
    PredictionErrorCode code{PredictionErrorCode::InvalidScope};
    std::optional<simulation::CheckpointRestoreError> checkpoint_error;
    std::optional<simulation::TickError> tick_error;

    bool operator==(const PredictionError&) const = default;
};

struct SimulationEventKeyHash {
    [[nodiscard]] std::size_t operator()(const simulation::SimulationEventKey& key) const noexcept;
};

} // namespace dots::prediction

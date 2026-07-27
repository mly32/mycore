#include "dots/prediction/scope.hpp"

#include "dots/prediction/mechanics.hpp"
#include "dots/simulation/movement.hpp"
#include "dots/simulation/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <utility>

namespace dots::prediction {
namespace {

constexpr auto kMaximumReplayHorizonTicks = std::uint64_t{1'024};
constexpr StateDomainMask kRequiredGlobalDomains = state_domain_bit(StateDomain::WorldRules) |
                                                   state_domain_bit(StateDomain::WorldTick) |
                                                   state_domain_bit(StateDomain::EntityAllocator);
constexpr StateDomainMask kCheckpointDomainMask =
    kAllStateDomains & ~state_domain_bit(StateDomain::EventJournal);
constexpr MechanicMask kKnownMechanics = mechanic_bit(PredictionMechanic::Movement) |
                                         mechanic_bit(PredictionMechanic::FoodConsumption) |
                                         mechanic_bit(PredictionMechanic::PlayerAbsorption) |
                                         mechanic_bit(PredictionMechanic::SplitMerge);

template <class Id> [[nodiscard]] bool is_strictly_sorted(const std::vector<Id>& values) {
    return std::adjacent_find(values.begin(), values.end(), [](Id lhs, Id rhs) {
               return lhs >= rhs;
           }) == values.end();
}

template <class Id> [[nodiscard]] bool contains(const std::vector<Id>& values, Id value) {
    return std::binary_search(values.begin(), values.end(), value);
}

[[nodiscard]] const simulation::OwnerCheckpoint*
find_owner(const simulation::WorldCheckpoint& checkpoint, simulation::PlayerOwnerId owner_id) {
    const auto owner = std::lower_bound(
        checkpoint.owners.begin(),
        checkpoint.owners.end(),
        owner_id,
        [](const simulation::OwnerCheckpoint& candidate, simulation::PlayerOwnerId id) {
            return candidate.owner_id < id;
        });
    return owner != checkpoint.owners.end() && owner->owner_id == owner_id ? &*owner : nullptr;
}

[[nodiscard]] const simulation::PlayerCheckpoint*
find_player(const simulation::WorldCheckpoint& checkpoint, simulation::EntityId entity_id) {
    const auto player = std::lower_bound(
        checkpoint.players.begin(),
        checkpoint.players.end(),
        entity_id,
        [](const simulation::PlayerCheckpoint& candidate, simulation::EntityId id) {
            return candidate.entity_id < id;
        });
    return player != checkpoint.players.end() && player->entity_id == entity_id ? &*player
                                                                                : nullptr;
}

[[nodiscard]] MechanicMask resolved_mechanics(MechanicMask requested) {
    auto result = requested;
    auto changed = true;
    while (changed) {
        changed = false;
        for (const auto& contract : kMechanicContracts) {
            if (!includes_mechanic(result, contract.mechanic)) {
                continue;
            }
            const auto expanded = result | contract.dependencies;
            changed = changed || expanded != result;
            result = expanded;
        }
    }
    return result;
}

[[nodiscard]] StateDomainMask required_domains(MechanicMask mechanics) {
    auto result = kRequiredGlobalDomains;
    for (const auto& contract : kMechanicContracts) {
        if (includes_mechanic(mechanics, contract.mechanic)) {
            result |= contract.reads | contract.writes;
        }
    }
    return result;
}

[[nodiscard]] bool domains_available(StateDomainMask required, const AuthorityCoverage& coverage) {
    const auto checkpoint_domains = required & kCheckpointDomainMask;
    return (coverage.available_domains & checkpoint_domains) == checkpoint_domains;
}

[[nodiscard]] bool has_remote_owner(const std::vector<simulation::PlayerOwnerId>& owners,
                                    const std::vector<simulation::PlayerOwnerId>& owned) {
    return std::any_of(owners.begin(), owners.end(), [&owned](simulation::PlayerOwnerId owner) {
        return !contains(owned, owner);
    });
}

[[nodiscard]] bool admits_player(const simulation::PlayerCheckpoint& player,
                                 const PredictionScope& scope) {
    if (contains(scope.player_ids, player.entity_id)) {
        return true;
    }
    return includes_mechanic(scope.mechanics, PredictionMechanic::SplitMerge) &&
           player.prediction_key && player.prediction_key->owner_id == player.owner_id &&
           contains(scope.owned_owner_ids, player.owner_id);
}

[[nodiscard]] std::variant<PredictionScope, ScopeBuildError>
make_owned_scope(const simulation::WorldCheckpoint& authority,
                 const PredictionRequest& request,
                 const std::vector<simulation::PlayerOwnerId>& owned_owner_ids,
                 MechanicMask requested_mechanics,
                 PredictionFallbackReason fallback_reason) {
    const auto mechanics = mechanic_bit(PredictionMechanic::Movement);
    const auto domains = required_domains(mechanics);
    if (!domains_available(domains, request.coverage)) {
        return ScopeBuildError::IncompleteOwnedState;
    }

    PredictionScope scope{
        .requested_profile = request.profile,
        .active_profile = PredictionProfile::OwnedMovement,
        .fallback_reason = fallback_reason,
        .requested_mechanics = requested_mechanics,
        .mechanics = mechanics,
        .required_domains = domains,
        .required_causal_channels = 0,
        .owned_owner_ids = owned_owner_ids,
        .owner_ids = owned_owner_ids,
        .player_ids = {},
        .food_ids = {},
        .rules = authority.rules,
        .replay_horizon = request.replay_horizon,
        .scope_epoch = request.scope_epoch,
    };
    for (const auto owner_id : owned_owner_ids) {
        const auto* owner = find_owner(authority, owner_id);
        if (owner == nullptr) {
            return ScopeBuildError::MissingOwnedOwner;
        }
        scope.player_ids.insert(
            scope.player_ids.end(), owner->player_ids.begin(), owner->player_ids.end());
    }
    std::sort(scope.player_ids.begin(), scope.player_ids.end());
    return scope;
}

[[nodiscard]] double squared_distance(mycore::math::Vector2 lhs,
                                      mycore::math::Vector2 rhs) noexcept {
    const auto delta_x = static_cast<double>(lhs.x) - static_cast<double>(rhs.x);
    const auto delta_y = static_cast<double>(lhs.y) - static_cast<double>(rhs.y);
    return (delta_x * delta_x) + (delta_y * delta_y);
}

[[nodiscard]] double travel_bound(const simulation::PlayerCheckpoint& player,
                                  const simulation::WorldRules& rules,
                                  mycore::time::TickDelta replay_horizon,
                                  MechanicMask mechanics) noexcept {
    const auto seconds =
        static_cast<double>(replay_horizon.value()) / static_cast<double>(simulation::kTickRateHz);
    const auto launch_speed = std::hypot(static_cast<double>(player.launch_velocity.x),
                                         static_cast<double>(player.launch_velocity.y));
    auto speed = static_cast<double>(rules.player_speed_units_per_second) + launch_speed;
    if (includes_mechanic(mechanics, PredictionMechanic::SplitMerge)) {
        speed += static_cast<double>(rules.child_launch_speed_units_per_second) +
                 static_cast<double>(rules.cohesion_speed_units_per_second);
    }
    return speed * seconds;
}

void expand_interaction_closure(const simulation::WorldCheckpoint& authority,
                                MechanicMask mechanics,
                                mycore::time::TickDelta replay_horizon,
                                std::set<simulation::PlayerOwnerId>& owners,
                                std::set<simulation::EntityId>& players,
                                std::set<simulation::EntityId>& food) {
    auto changed = true;
    while (changed) {
        changed = false;
        for (const auto owner_id : owners) {
            const auto* owner = find_owner(authority, owner_id);
            if (owner == nullptr) {
                continue;
            }
            for (const auto player_id : owner->player_ids) {
                changed = players.insert(player_id).second || changed;
            }
        }

        auto potential_island_mass = 0.0;
        for (const auto player_id : players) {
            if (const auto* player = find_player(authority, player_id)) {
                potential_island_mass += static_cast<double>(player->mass);
            }
        }
        potential_island_mass +=
            static_cast<double>(food.size()) * static_cast<double>(authority.rules.food_mass);
        const auto potential_radius = std::sqrt(potential_island_mass);

        const auto included_players =
            std::vector<simulation::EntityId>{players.begin(), players.end()};
        for (const auto player_id : included_players) {
            const auto* player = find_player(authority, player_id);
            if (player == nullptr) {
                continue;
            }
            changed = owners.insert(player->owner_id).second || changed;
            const auto source_reach =
                travel_bound(*player, authority.rules, replay_horizon, mechanics);

            if (includes_mechanic(mechanics, PredictionMechanic::FoodConsumption)) {
                const auto food_radius =
                    static_cast<double>(simulation::radius_for_mass(authority.rules.food_mass));
                for (const auto& candidate : authority.food) {
                    const auto reach = potential_radius + food_radius + source_reach;
                    if (squared_distance(player->position, candidate.position) <= reach * reach) {
                        changed = food.insert(candidate.entity_id).second || changed;
                    }
                }
            }

            if (includes_mechanic(mechanics, PredictionMechanic::PlayerAbsorption)) {
                for (const auto& candidate : authority.players) {
                    if (candidate.entity_id == player_id) {
                        continue;
                    }
                    const auto candidate_reach =
                        travel_bound(candidate, authority.rules, replay_horizon, mechanics);
                    const auto reach =
                        potential_radius +
                        static_cast<double>(simulation::radius_for_mass(candidate.mass)) +
                        source_reach + candidate_reach;
                    if (squared_distance(player->position, candidate.position) <= reach * reach) {
                        changed = players.insert(candidate.entity_id).second || changed;
                        changed = owners.insert(candidate.owner_id).second || changed;
                    }
                }
            }
        }
    }
}

[[nodiscard]] PredictionError error(PredictionErrorCode code) {
    return PredictionError{.code = code, .checkpoint_error = {}, .tick_error = {}};
}

[[nodiscard]] bool is_known_profile(PredictionProfile profile) noexcept {
    switch (profile) {
    case PredictionProfile::InteractionClosure:
    case PredictionProfile::FullReplicated:
    case PredictionProfile::OwnedMovement:
        return true;
    default:
        return false;
    }
}

} // namespace

ScopeBuildResult build_prediction_scope(const simulation::WorldCheckpoint& authority,
                                        const PredictionRequest& request) {
    simulation::World validator;
    if (validator.restore(authority)) {
        return ScopeBuildError::InvalidCheckpoint;
    }
    if (!is_known_profile(request.profile) || !request.scope_epoch.is_valid() ||
        request.replay_horizon.value() == 0 ||
        request.replay_horizon.value() > kMaximumReplayHorizonTicks ||
        request.owned_owner_ids.empty() || request.mechanics == 0 ||
        (request.mechanics & ~kKnownMechanics) != 0U) {
        return ScopeBuildError::InvalidRequest;
    }

    auto owned_owner_ids = request.owned_owner_ids;
    std::sort(owned_owner_ids.begin(), owned_owner_ids.end());
    if (!is_strictly_sorted(owned_owner_ids) ||
        std::any_of(owned_owner_ids.begin(), owned_owner_ids.end(), [](auto owner_id) {
            return !owner_id.is_valid();
        })) {
        return ScopeBuildError::InvalidRequest;
    }
    for (const auto owner_id : owned_owner_ids) {
        if (find_owner(authority, owner_id) == nullptr) {
            return ScopeBuildError::MissingOwnedOwner;
        }
    }

    const auto mechanics = resolved_mechanics(request.mechanics);
    for (const auto& contract : kMechanicContracts) {
        if (includes_mechanic(mechanics, contract.mechanic) && !contract.implemented) {
            return ScopeBuildError::UnsupportedMechanic;
        }
    }

    if (request.profile == PredictionProfile::OwnedMovement) {
        return make_owned_scope(
            authority, request, owned_owner_ids, request.mechanics, PredictionFallbackReason::None);
    }

    const auto domains = required_domains(mechanics);
    const auto incomplete_profile = !domains_available(domains, request.coverage) ||
                                    (request.profile == PredictionProfile::FullReplicated &&
                                     !request.coverage.complete_entity_set) ||
                                    (request.profile == PredictionProfile::InteractionClosure &&
                                     !request.coverage.complete_spatial_neighborhood);
    if (incomplete_profile) {
        return make_owned_scope(authority,
                                request,
                                owned_owner_ids,
                                request.mechanics,
                                PredictionFallbackReason::IncompleteClosure);
    }

    std::set<simulation::PlayerOwnerId> owners{owned_owner_ids.begin(), owned_owner_ids.end()};
    std::set<simulation::EntityId> players;
    std::set<simulation::EntityId> food;
    if (request.profile == PredictionProfile::FullReplicated) {
        for (const auto& owner : authority.owners) {
            owners.insert(owner.owner_id);
        }
        for (const auto& player : authority.players) {
            players.insert(player.entity_id);
        }
        for (const auto& item : authority.food) {
            food.insert(item.entity_id);
        }
    } else {
        expand_interaction_closure(
            authority, mechanics, request.replay_horizon, owners, players, food);
    }

    const auto owner_ids = std::vector<simulation::PlayerOwnerId>{owners.begin(), owners.end()};
    auto required_channels = CausalChannelMask{};
    if (has_remote_owner(owner_ids, owned_owner_ids)) {
        required_channels |= causal_channel_bit(CausalChannel::RemoteMovement);
    }
    if ((request.coverage.available_causal_channels & required_channels) != required_channels) {
        return make_owned_scope(authority,
                                request,
                                owned_owner_ids,
                                request.mechanics,
                                PredictionFallbackReason::IncompleteClosure);
    }

    auto player_ids = std::vector<simulation::EntityId>{players.begin(), players.end()};
    auto food_ids = std::vector<simulation::EntityId>{food.begin(), food.end()};
    return PredictionScope{
        .requested_profile = request.profile,
        .active_profile = request.profile,
        .fallback_reason = PredictionFallbackReason::None,
        .requested_mechanics = request.mechanics,
        .mechanics = mechanics,
        .required_domains = domains,
        .required_causal_channels = required_channels,
        .owned_owner_ids = std::move(owned_owner_ids),
        .owner_ids = owner_ids,
        .player_ids = std::move(player_ids),
        .food_ids = std::move(food_ids),
        .rules = authority.rules,
        .replay_horizon = request.replay_horizon,
        .scope_epoch = request.scope_epoch,
    };
}

CheckpointProjectionResult project_checkpoint(const simulation::WorldCheckpoint& authority,
                                              const PredictionScope& scope) {
    if (authority.rules != scope.rules) {
        return error(PredictionErrorCode::IncompatibleRules);
    }
    simulation::World validator;
    if (const auto restore_error = validator.restore(authority)) {
        return PredictionError{
            .code = PredictionErrorCode::CheckpointRestoreFailed,
            .checkpoint_error = restore_error,
            .tick_error = {},
        };
    }
    if (!scope.scope_epoch.is_valid() || !is_strictly_sorted(scope.owned_owner_ids) ||
        !is_strictly_sorted(scope.owner_ids) || !is_strictly_sorted(scope.player_ids) ||
        !is_strictly_sorted(scope.food_ids)) {
        return error(PredictionErrorCode::InvalidScope);
    }

    simulation::WorldCheckpoint projected{
        .rules = authority.rules,
        .tick = authority.tick,
        .next_entity_id = authority.next_entity_id,
        .owners = {},
        .players = {},
        .food = {},
    };
    for (const auto& owner : authority.owners) {
        if (!contains(scope.owner_ids, owner.owner_id)) {
            if (scope.active_profile == PredictionProfile::FullReplicated) {
                return error(PredictionErrorCode::CheckpointOutsideScope);
            }
            continue;
        }
        if (std::any_of(owner.player_ids.begin(),
                        owner.player_ids.end(),
                        [&authority, &scope](auto player_id) {
                            const auto* player = find_player(authority, player_id);
                            return player == nullptr || !admits_player(*player, scope);
                        })) {
            return error(PredictionErrorCode::CheckpointOutsideScope);
        }
        projected.owners.push_back(owner);
    }
    for (const auto& player : authority.players) {
        if (admits_player(player, scope)) {
            if (!contains(scope.owner_ids, player.owner_id)) {
                return error(PredictionErrorCode::CheckpointOutsideScope);
            }
            projected.players.push_back(player);
        } else if (scope.active_profile == PredictionProfile::FullReplicated) {
            return error(PredictionErrorCode::CheckpointOutsideScope);
        }
    }
    for (const auto& item : authority.food) {
        if (contains(scope.food_ids, item.entity_id)) {
            projected.food.push_back(item);
        } else if (scope.active_profile == PredictionProfile::FullReplicated) {
            return error(PredictionErrorCode::CheckpointOutsideScope);
        }
    }

    simulation::World projected_validator;
    if (const auto restore_error = projected_validator.restore(projected)) {
        return PredictionError{
            .code = PredictionErrorCode::CheckpointRestoreFailed,
            .checkpoint_error = restore_error,
            .tick_error = {},
        };
    }
    return projected;
}

} // namespace dots::prediction

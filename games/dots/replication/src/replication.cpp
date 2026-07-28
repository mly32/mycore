#include "dots/replication/replication.hpp"

#include "dots/prediction/model.hpp"
#include "dots/protocol/codec.hpp"
#include "dots/simulation/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace dots::replication {
namespace {

[[nodiscard]] bool valid_entity(const protocol::EntityState& entity) noexcept {
    const auto known_kind =
        entity.kind == protocol::EntityKind::Player || entity.kind == protocol::EntityKind::Food;
    const auto valid_owner =
        (entity.kind == protocol::EntityKind::Player) == entity.owner_id.is_valid();
    return entity.entity_id.is_valid() && known_kind && valid_owner &&
           std::isfinite(entity.position_x) && std::isfinite(entity.position_y) &&
           std::isfinite(entity.mass) && entity.mass > 0.0F &&
           std::isfinite(entity.launch_velocity_x) && std::isfinite(entity.launch_velocity_y);
}

[[nodiscard]] bool tick_fits_protocol(mycore::time::Tick tick) noexcept {
    return tick.value() <= std::numeric_limits<std::uint32_t>::max();
}

} // namespace

protocol::EntityId to_protocol(simulation::EntityId id) noexcept {
    return protocol::EntityId{id.value()};
}

protocol::PlayerOwnerId to_protocol(simulation::PlayerOwnerId id) noexcept {
    return protocol::PlayerOwnerId{id.value()};
}

protocol::WorldRules to_protocol(const simulation::WorldRules& rules) noexcept {
    return {
        .initial_player_mass = rules.initial_player_mass,
        .food_mass = rules.food_mass,
        .spatial_grid_cell_size = rules.spatial_grid_cell_size,
        .player_speed_units_per_second = rules.player_speed_units_per_second,
        .split_recast_ticks = rules.split_recast_ticks,
        .merge_delay_ticks = rules.merge_delay_ticks,
        .maximum_pieces_per_owner = rules.maximum_pieces_per_owner,
        .minimum_split_mass = rules.minimum_split_mass,
        .child_launch_speed_units_per_second = rules.child_launch_speed_units_per_second,
        .launch_decay_units_per_second_squared = rules.launch_decay_units_per_second_squared,
        .cohesion_speed_units_per_second = rules.cohesion_speed_units_per_second,
    };
}

simulation::InputCommandId to_simulation(protocol::InputSequenceId id) noexcept {
    return simulation::InputCommandId{id.value()};
}

simulation::WorldRules to_simulation(const protocol::WorldRules& rules) noexcept {
    return {
        .initial_player_mass = rules.initial_player_mass,
        .food_mass = rules.food_mass,
        .spatial_grid_cell_size = rules.spatial_grid_cell_size,
        .player_speed_units_per_second = rules.player_speed_units_per_second,
        .split_recast_ticks = rules.split_recast_ticks,
        .merge_delay_ticks = rules.merge_delay_ticks,
        .maximum_pieces_per_owner = rules.maximum_pieces_per_owner,
        .minimum_split_mass = rules.minimum_split_mass,
        .child_launch_speed_units_per_second = rules.child_launch_speed_units_per_second,
        .launch_decay_units_per_second_squared = rules.launch_decay_units_per_second_squared,
        .cohesion_speed_units_per_second = rules.cohesion_speed_units_per_second,
    };
}

simulation::SimulationEvent to_simulation(const protocol::AuthorityEvent& event) {
    return std::visit(
        [](const auto& value) -> simulation::SimulationEvent {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, protocol::FoodConsumed>) {
                return simulation::FoodConsumed{
                    .tick = mycore::time::Tick{value.server_tick},
                    .food_entity_id = simulation::EntityId{value.food_entity_id.value()},
                    .consumer_entity_id = simulation::EntityId{value.consumer_entity_id.value()},
                    .consumer_owner_id = simulation::PlayerOwnerId{value.consumer_owner_id.value()},
                    .transferred_mass = value.transferred_mass,
                };
            } else if constexpr (std::is_same_v<Event, protocol::PlayerAbsorbed>) {
                return simulation::PlayerAbsorbed{
                    .tick = mycore::time::Tick{value.server_tick},
                    .absorber_entity_id = simulation::EntityId{value.absorber_entity_id.value()},
                    .victim_entity_id = simulation::EntityId{value.victim_entity_id.value()},
                    .absorber_owner_id = simulation::PlayerOwnerId{value.absorber_owner_id.value()},
                    .victim_owner_id = simulation::PlayerOwnerId{value.victim_owner_id.value()},
                    .transferred_mass = value.transferred_mass,
                };
            } else if constexpr (std::is_same_v<Event, protocol::PlayerSplit>) {
                return simulation::PlayerSplit{
                    .tick = mycore::time::Tick{value.server_tick},
                    .owner_id = simulation::PlayerOwnerId{value.owner_id.value()},
                    .input_id = simulation::InputCommandId{value.input_id.value()},
                    .child_ordinal = value.child_ordinal,
                    .parent_entity_id = simulation::EntityId{value.parent_entity_id.value()},
                    .child_entity_id = simulation::EntityId{value.child_entity_id.value()},
                    .parent_mass = value.parent_mass,
                    .child_mass = value.child_mass,
                };
            } else {
                return simulation::PiecesMerged{
                    .tick = mycore::time::Tick{value.server_tick},
                    .owner_id = simulation::PlayerOwnerId{value.owner_id.value()},
                    .survivor_entity_id = simulation::EntityId{value.survivor_entity_id.value()},
                    .consumed_entity_id = simulation::EntityId{value.consumed_entity_id.value()},
                    .combined_mass = value.combined_mass,
                };
            }
        },
        event);
}

AuthorityEventBuildResult to_protocol(const simulation::SimulationEvent& event) {
    return std::visit(
        [](const auto& value) -> AuthorityEventBuildResult {
            if (!tick_fits_protocol(value.tick)) {
                return AuthorityEventBuildError::TickOutOfRange;
            }
            const auto server_tick = static_cast<std::uint32_t>(value.tick.value());
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, simulation::FoodConsumed>) {
                return protocol::AuthorityEvent{protocol::FoodConsumed{
                    .server_tick = server_tick,
                    .food_entity_id = to_protocol(value.food_entity_id),
                    .consumer_entity_id = to_protocol(value.consumer_entity_id),
                    .consumer_owner_id = to_protocol(value.consumer_owner_id),
                    .transferred_mass = value.transferred_mass,
                }};
            } else if constexpr (std::is_same_v<Event, simulation::PlayerAbsorbed>) {
                return protocol::AuthorityEvent{protocol::PlayerAbsorbed{
                    .server_tick = server_tick,
                    .absorber_entity_id = to_protocol(value.absorber_entity_id),
                    .victim_entity_id = to_protocol(value.victim_entity_id),
                    .absorber_owner_id = to_protocol(value.absorber_owner_id),
                    .victim_owner_id = to_protocol(value.victim_owner_id),
                    .transferred_mass = value.transferred_mass,
                }};
            } else if constexpr (std::is_same_v<Event, simulation::PlayerSplit>) {
                return protocol::AuthorityEvent{protocol::PlayerSplit{
                    .server_tick = server_tick,
                    .owner_id = to_protocol(value.owner_id),
                    .input_id = protocol::InputSequenceId{value.input_id.value()},
                    .child_ordinal = value.child_ordinal,
                    .parent_entity_id = to_protocol(value.parent_entity_id),
                    .child_entity_id = to_protocol(value.child_entity_id),
                    .parent_mass = value.parent_mass,
                    .child_mass = value.child_mass,
                }};
            } else {
                return protocol::AuthorityEvent{protocol::PiecesMerged{
                    .server_tick = server_tick,
                    .owner_id = to_protocol(value.owner_id),
                    .survivor_entity_id = to_protocol(value.survivor_entity_id),
                    .consumed_entity_id = to_protocol(value.consumed_entity_id),
                    .combined_mass = value.combined_mass,
                }};
            }
        },
        event);
}

SnapshotBuildResult
build_full_snapshot(const simulation::World& world,
                    protocol::SnapshotId snapshot_id,
                    protocol::InputSequenceId last_processed,
                    std::uint8_t pending_input_count,
                    protocol::RecipientSessionState recipient,
                    std::vector<protocol::AuthorityReceipt> authority_receipts) {
    if (!snapshot_id.is_valid()) {
        return SnapshotBuildError::InvalidSnapshotId;
    }
    const auto checkpoint = world.checkpoint();
    if (!tick_fits_protocol(checkpoint.tick)) {
        return SnapshotBuildError::TickOutOfRange;
    }
    if (pending_input_count > protocol::kMaximumPendingInputCount ||
        authority_receipts.size() > protocol::kMaximumAuthorityReceiptsPerSnapshot) {
        return SnapshotBuildError::InvalidWorldState;
    }

    protocol::FullSnapshot snapshot{
        .snapshot_id = snapshot_id,
        .server_tick = static_cast<std::uint32_t>(checkpoint.tick.value()),
        .last_processed_input_id = last_processed,
        .pending_input_count = pending_input_count,
        .checkpoint_schema_id = protocol::kCheckpointSchemaId,
        .checkpoint_digest = prediction::checkpoint_digest(checkpoint).value,
        .next_entity_id = protocol::EntityId{checkpoint.next_entity_id},
        .recipient = std::move(recipient),
        .owners = {},
        .entities = {},
        .authority_receipts = std::move(authority_receipts),
    };
    snapshot.owners.reserve(checkpoint.owners.size());
    for (const auto& owner : checkpoint.owners) {
        if (!tick_fits_protocol(owner.split_cooldown_end_tick)) {
            return SnapshotBuildError::TickOutOfRange;
        }
        snapshot.owners.push_back({
            .owner_id = to_protocol(owner.owner_id),
            .movement_x = owner.movement.x,
            .movement_y = owner.movement.y,
            .last_non_zero_movement_x = owner.last_non_zero_movement.x,
            .last_non_zero_movement_y = owner.last_non_zero_movement.y,
            .last_input_id = protocol::InputSequenceId{owner.last_input_id.value()},
            .split_cooldown_end_tick =
                static_cast<std::uint32_t>(owner.split_cooldown_end_tick.value()),
        });
    }

    snapshot.entities.reserve(checkpoint.players.size() + checkpoint.food.size());
    for (const auto& player : checkpoint.players) {
        if (!tick_fits_protocol(player.merge_eligible_tick)) {
            return SnapshotBuildError::TickOutOfRange;
        }
        std::optional<protocol::PredictionKey> prediction_key;
        if (player.prediction_key) {
            prediction_key = protocol::PredictionKey{
                .owner_id = to_protocol(player.prediction_key->owner_id),
                .input_id = protocol::InputSequenceId{player.prediction_key->input_id.value()},
                .child_ordinal = player.prediction_key->child_ordinal,
            };
        }
        snapshot.entities.push_back({
            .entity_id = to_protocol(player.entity_id),
            .kind = protocol::EntityKind::Player,
            .owner_id = to_protocol(player.owner_id),
            .position_x = player.position.x,
            .position_y = player.position.y,
            .mass = player.mass,
            .launch_velocity_x = player.launch_velocity.x,
            .launch_velocity_y = player.launch_velocity.y,
            .merge_eligible_tick = static_cast<std::uint32_t>(player.merge_eligible_tick.value()),
            .prediction_key = prediction_key,
        });
    }
    for (const auto& food : checkpoint.food) {
        snapshot.entities.push_back({
            .entity_id = to_protocol(food.entity_id),
            .kind = protocol::EntityKind::Food,
            .owner_id = protocol::PlayerOwnerId::invalid(),
            .position_x = food.position.x,
            .position_y = food.position.y,
            .mass = checkpoint.rules.food_mass,
            .prediction_key = std::nullopt,
        });
    }
    std::sort(snapshot.entities.begin(),
              snapshot.entities.end(),
              [](const protocol::EntityState& lhs, const protocol::EntityState& rhs) {
                  return lhs.entity_id < rhs.entity_id;
              });
    if (protocol::validate(snapshot)) {
        return SnapshotBuildError::InvalidWorldState;
    }
    return snapshot;
}

CheckpointHydrationResult hydrate_checkpoint(const protocol::FullSnapshot& snapshot,
                                             const protocol::WorldRules& authoritative_rules) {
    if (protocol::validate(snapshot)) {
        return CheckpointHydrationError::InvalidSnapshot;
    }

    simulation::WorldCheckpoint checkpoint{
        .rules = to_simulation(authoritative_rules),
        .tick = mycore::time::Tick{snapshot.server_tick},
        .next_entity_id = snapshot.next_entity_id.value(),
        .owners = {},
        .players = {},
        .food = {},
    };
    checkpoint.owners.reserve(snapshot.owners.size());
    for (const auto& owner : snapshot.owners) {
        checkpoint.owners.push_back({
            .owner_id = simulation::PlayerOwnerId{owner.owner_id.value()},
            .player_ids = {},
            .movement = {owner.movement_x, owner.movement_y},
            .last_non_zero_movement = {owner.last_non_zero_movement_x,
                                       owner.last_non_zero_movement_y},
            .last_input_id = simulation::InputCommandId{owner.last_input_id.value()},
            .split_cooldown_end_tick = mycore::time::Tick{owner.split_cooldown_end_tick},
        });
    }

    checkpoint.players.reserve(snapshot.entities.size());
    checkpoint.food.reserve(snapshot.entities.size());
    for (const auto& entity : snapshot.entities) {
        if (entity.kind == protocol::EntityKind::Food) {
            if (entity.mass != checkpoint.rules.food_mass) {
                return CheckpointHydrationError::InvalidFoodMass;
            }
            checkpoint.food.push_back({
                .entity_id = simulation::EntityId{entity.entity_id.value()},
                .position = {entity.position_x, entity.position_y},
            });
            continue;
        }
        std::optional<simulation::PredictionKey> prediction_key;
        if (entity.prediction_key) {
            prediction_key = simulation::PredictionKey{
                .owner_id = simulation::PlayerOwnerId{entity.prediction_key->owner_id.value()},
                .input_id = simulation::InputCommandId{entity.prediction_key->input_id.value()},
                .child_ordinal = entity.prediction_key->child_ordinal,
            };
        }
        checkpoint.players.push_back({
            .entity_id = simulation::EntityId{entity.entity_id.value()},
            .owner_id = simulation::PlayerOwnerId{entity.owner_id.value()},
            .position = {entity.position_x, entity.position_y},
            .mass = entity.mass,
            .launch_velocity = {entity.launch_velocity_x, entity.launch_velocity_y},
            .merge_eligible_tick = mycore::time::Tick{entity.merge_eligible_tick},
            .prediction_key = prediction_key,
        });
        const auto owner =
            std::lower_bound(checkpoint.owners.begin(),
                             checkpoint.owners.end(),
                             entity.owner_id.value(),
                             [](const simulation::OwnerCheckpoint& value, std::uint32_t owner_id) {
                                 return value.owner_id.value() < owner_id;
                             });
        if (owner == checkpoint.owners.end() ||
            owner->owner_id.value() != entity.owner_id.value()) {
            return CheckpointHydrationError::InvalidSnapshot;
        }
        owner->player_ids.push_back(simulation::EntityId{entity.entity_id.value()});
    }

    simulation::World validated_world;
    if (validated_world.restore(checkpoint)) {
        return CheckpointHydrationError::RestoreFailed;
    }
    if (prediction::checkpoint_digest(checkpoint).value != snapshot.checkpoint_digest) {
        return CheckpointHydrationError::DigestMismatch;
    }
    return checkpoint;
}

SnapshotApplyResult ReplicatedWorld::apply(const protocol::FullSnapshot& snapshot) {
    if (!snapshot.snapshot_id.is_valid() ||
        snapshot.pending_input_count > protocol::kMaximumPendingInputCount) {
        return SnapshotApplyResult::Invalid;
    }
    if (snapshot_id_.is_valid() && snapshot.snapshot_id <= snapshot_id_) {
        return SnapshotApplyResult::Stale;
    }
    if (snapshot_id_.is_valid() && snapshot.server_tick < server_tick_) {
        return SnapshotApplyResult::Invalid;
    }
    if (protocol::validate(snapshot)) {
        return SnapshotApplyResult::Invalid;
    }

    std::unordered_set<std::uint32_t> ids;
    ids.reserve(snapshot.entities.size());
    for (const auto& entity : snapshot.entities) {
        if (!valid_entity(entity) || !ids.insert(entity.entity_id.value()).second) {
            return SnapshotApplyResult::Invalid;
        }
    }

    auto next_receipts = authority_receipts_;
    auto next_acknowledgement = authority_receipt_acknowledgement_;
    for (const auto& receipt : snapshot.authority_receipts) {
        const auto existing =
            std::lower_bound(next_receipts.begin(),
                             next_receipts.end(),
                             receipt.sequence_id,
                             [](const protocol::AuthorityReceipt& value,
                                protocol::AuthorityReceiptSequenceId sequence_id) {
                                 return value.sequence_id < sequence_id;
                             });
        if (existing != next_receipts.end() && existing->sequence_id == receipt.sequence_id) {
            if (*existing != receipt) {
                return SnapshotApplyResult::Invalid;
            }
            continue;
        }
        const auto expected =
            next_acknowledgement.is_valid() ? next_acknowledgement.value() + 1U : std::uint32_t{0};
        if (receipt.sequence_id.value() != expected) {
            return SnapshotApplyResult::Invalid;
        }
        next_receipts.insert(existing, receipt);
        next_acknowledgement = receipt.sequence_id;
    }

    snapshot_id_ = snapshot.snapshot_id;
    server_tick_ = snapshot.server_tick;
    last_processed_input_id_ = snapshot.last_processed_input_id;
    pending_input_count_ = snapshot.pending_input_count;
    recipient_ = snapshot.recipient;
    entities_ = snapshot.entities;
    authority_receipts_ = std::move(next_receipts);
    authority_receipt_acknowledgement_ = next_acknowledgement;
    return SnapshotApplyResult::Applied;
}

const protocol::EntityState* ReplicatedWorld::find(protocol::EntityId entity_id) const noexcept {
    const auto iterator =
        std::lower_bound(entities_.begin(),
                         entities_.end(),
                         entity_id,
                         [](const protocol::EntityState& entity, protocol::EntityId value) {
                             return entity.entity_id < value;
                         });
    return iterator != entities_.end() && iterator->entity_id == entity_id ? &*iterator : nullptr;
}

std::span<const protocol::EntityState> ReplicatedWorld::entities() const noexcept {
    return entities_;
}

std::size_t ReplicatedWorld::player_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(entities_.begin(), entities_.end(), [](const protocol::EntityState& entity) {
            return entity.kind == protocol::EntityKind::Player;
        }));
}

std::size_t ReplicatedWorld::food_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(entities_.begin(), entities_.end(), [](const protocol::EntityState& entity) {
            return entity.kind == protocol::EntityKind::Food;
        }));
}

std::uint32_t ReplicatedWorld::server_tick() const noexcept {
    return server_tick_;
}

protocol::SnapshotId ReplicatedWorld::snapshot_id() const noexcept {
    return snapshot_id_;
}

protocol::InputSequenceId ReplicatedWorld::last_processed_input_id() const noexcept {
    return last_processed_input_id_;
}

std::uint8_t ReplicatedWorld::pending_input_count() const noexcept {
    return pending_input_count_;
}

const protocol::RecipientSessionState& ReplicatedWorld::recipient() const noexcept {
    return recipient_;
}

protocol::AuthorityReceiptSequenceId
ReplicatedWorld::authority_receipt_acknowledgement() const noexcept {
    return authority_receipt_acknowledgement_;
}

std::span<const protocol::AuthorityReceipt> ReplicatedWorld::authority_receipts() const noexcept {
    return authority_receipts_;
}

} // namespace dots::replication

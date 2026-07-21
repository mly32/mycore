#include "dots/replication/replication.hpp"

#include "dots/simulation/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <variant>

namespace dots::replication {
namespace {

[[nodiscard]] bool valid_entity(const protocol::EntityState& entity) noexcept {
    const auto known_kind =
        entity.kind == protocol::EntityKind::Player || entity.kind == protocol::EntityKind::Food;
    return entity.entity_id.is_valid() && known_kind && std::isfinite(entity.position_x) &&
           std::isfinite(entity.position_y) && std::isfinite(entity.mass) && entity.mass > 0.0F;
}

} // namespace

protocol::EntityId to_protocol(simulation::EntityId id) noexcept {
    return protocol::EntityId{id.value()};
}

simulation::InputCommandId to_simulation(protocol::InputSequenceId id) noexcept {
    return simulation::InputCommandId{id.value()};
}

SnapshotBuildResult build_full_snapshot(const simulation::World& world,
                                        protocol::SnapshotId snapshot_id,
                                        protocol::InputSequenceId last_processed,
                                        std::uint8_t pending_input_count) {
    if (!snapshot_id.is_valid()) {
        return SnapshotBuildError::InvalidSnapshotId;
    }
    if (world.tick().value() > std::numeric_limits<std::uint32_t>::max()) {
        return SnapshotBuildError::TickOutOfRange;
    }
    if (pending_input_count > protocol::kMaximumPendingInputCount) {
        return SnapshotBuildError::InvalidWorldState;
    }

    protocol::FullSnapshot snapshot{
        .snapshot_id = snapshot_id,
        .server_tick = static_cast<std::uint32_t>(world.tick().value()),
        .last_processed_input_id = last_processed,
        .pending_input_count = pending_input_count,
        .entities = {},
    };
    snapshot.entities.reserve(world.player_count() + world.food_count());
    const auto append = [&world, &snapshot](std::span<const simulation::EntityId> ids,
                                            protocol::EntityKind kind) {
        for (const auto id : ids) {
            const auto position = world.position(id);
            const auto mass = world.mass(id);
            if (!position || !mass || !std::isfinite(position->x) || !std::isfinite(position->y) ||
                !std::isfinite(*mass) || *mass <= 0.0F) {
                return false;
            }
            snapshot.entities.push_back({
                .entity_id = to_protocol(id),
                .kind = kind,
                .position_x = position->x,
                .position_y = position->y,
                .mass = *mass,
            });
        }
        return true;
    };
    if (!append(world.player_ids(), protocol::EntityKind::Player) ||
        !append(world.food_ids(), protocol::EntityKind::Food)) {
        return SnapshotBuildError::InvalidWorldState;
    }
    std::sort(snapshot.entities.begin(),
              snapshot.entities.end(),
              [](const protocol::EntityState& lhs, const protocol::EntityState& rhs) {
                  return lhs.entity_id < rhs.entity_id;
              });
    return snapshot;
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

    std::unordered_set<std::uint32_t> ids;
    ids.reserve(snapshot.entities.size());
    for (const auto& entity : snapshot.entities) {
        if (!valid_entity(entity) || !ids.insert(entity.entity_id.value()).second) {
            return SnapshotApplyResult::Invalid;
        }
    }

    snapshot_id_ = snapshot.snapshot_id;
    server_tick_ = snapshot.server_tick;
    last_processed_input_id_ = snapshot.last_processed_input_id;
    pending_input_count_ = snapshot.pending_input_count;
    entities_ = snapshot.entities;
    std::sort(entities_.begin(),
              entities_.end(),
              [](const protocol::EntityState& lhs, const protocol::EntityState& rhs) {
                  return lhs.entity_id < rhs.entity_id;
              });
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

} // namespace dots::replication

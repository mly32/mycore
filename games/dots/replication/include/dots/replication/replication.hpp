#pragma once

#include "dots/protocol/messages.hpp"
#include "dots/simulation/ids.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace dots::simulation {
class World;
}

namespace dots::replication {

enum class SnapshotBuildError : std::uint8_t {
    InvalidSnapshotId,
    TickOutOfRange,
    InvalidWorldState,
};

using SnapshotBuildResult = std::variant<protocol::FullSnapshot, SnapshotBuildError>;

enum class SnapshotApplyResult : std::uint8_t {
    Applied,
    Stale,
    Invalid,
};

[[nodiscard]] protocol::EntityId to_protocol(simulation::EntityId id) noexcept;
[[nodiscard]] protocol::PlayerOwnerId to_protocol(simulation::PlayerOwnerId id) noexcept;
[[nodiscard]] simulation::InputCommandId to_simulation(protocol::InputSequenceId id) noexcept;

[[nodiscard]] SnapshotBuildResult build_full_snapshot(const simulation::World& world,
                                                      protocol::SnapshotId snapshot_id,
                                                      protocol::InputSequenceId last_processed,
                                                      std::uint8_t pending_input_count,
                                                      protocol::RecipientSessionState recipient);

class ReplicatedWorld {
public:
    [[nodiscard]] SnapshotApplyResult apply(const protocol::FullSnapshot& snapshot);
    [[nodiscard]] const protocol::EntityState* find(protocol::EntityId entity_id) const noexcept;
    [[nodiscard]] std::span<const protocol::EntityState> entities() const noexcept;
    [[nodiscard]] std::size_t player_count() const noexcept;
    [[nodiscard]] std::size_t food_count() const noexcept;
    [[nodiscard]] std::uint32_t server_tick() const noexcept;
    [[nodiscard]] protocol::SnapshotId snapshot_id() const noexcept;
    [[nodiscard]] protocol::InputSequenceId last_processed_input_id() const noexcept;
    [[nodiscard]] std::uint8_t pending_input_count() const noexcept;
    [[nodiscard]] const protocol::RecipientSessionState& recipient() const noexcept;

private:
    protocol::SnapshotId snapshot_id_;
    std::uint32_t server_tick_{};
    protocol::InputSequenceId last_processed_input_id_;
    std::uint8_t pending_input_count_{};
    protocol::RecipientSessionState recipient_;
    std::vector<protocol::EntityState> entities_;
};

} // namespace dots::replication

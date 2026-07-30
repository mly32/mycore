#pragma once

#include "dots/protocol/messages.hpp"
#include "dots/simulation/ids.hpp"
#include "dots/simulation/tick.hpp"
#include "dots/simulation/world_state.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
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

enum class CheckpointHydrationError : std::uint8_t {
    InvalidSnapshot,
    InvalidFoodMass,
    RestoreFailed,
    DigestMismatch,
};

using CheckpointHydrationResult =
    std::variant<simulation::WorldCheckpoint, CheckpointHydrationError>;

enum class AuthorityEventBuildError : std::uint8_t {
    TickOutOfRange,
};

using AuthorityEventBuildResult = std::variant<protocol::AuthorityEvent, AuthorityEventBuildError>;

enum class SnapshotApplyResult : std::uint8_t {
    Applied,
    Stale,
    Invalid,
};

enum class AuthorityReceiptApplyError : std::uint8_t {
    InvalidRetirement,
    SequenceGap,
    ConflictingReceipt,
    DuplicateEventKey,
    CapacityExceeded,
};

struct AuthorityReceiptDelta {
    std::vector<protocol::AuthorityReceipt> receipts;
    std::vector<simulation::SimulationEventKey> externally_retired_keys;
    std::optional<std::uint32_t> complete_coverage_through_server_tick;
};

using AuthorityReceiptApplyResult = std::variant<AuthorityReceiptDelta, AuthorityReceiptApplyError>;

[[nodiscard]] protocol::EntityId to_protocol(simulation::EntityId id) noexcept;
[[nodiscard]] protocol::PlayerOwnerId to_protocol(simulation::PlayerOwnerId id) noexcept;
[[nodiscard]] protocol::WorldRules to_protocol(const simulation::WorldRules& rules) noexcept;
[[nodiscard]] simulation::InputCommandId to_simulation(protocol::InputSequenceId id) noexcept;
[[nodiscard]] simulation::WorldRules to_simulation(const protocol::WorldRules& rules) noexcept;
[[nodiscard]] simulation::SimulationEvent to_simulation(const protocol::AuthorityEvent& event);
[[nodiscard]] AuthorityEventBuildResult to_protocol(const simulation::SimulationEvent& event);

[[nodiscard]] SnapshotBuildResult
build_full_snapshot(const simulation::World& world,
                    protocol::SnapshotId snapshot_id,
                    protocol::InputSequenceId last_processed,
                    std::uint8_t pending_input_count,
                    protocol::RecipientSessionState recipient,
                    std::vector<protocol::AuthorityReceipt> authority_receipts = {},
                    protocol::AuthorityReceiptSequenceId authority_receipts_retired_through = {});

[[nodiscard]] CheckpointHydrationResult
hydrate_checkpoint(const protocol::FullSnapshot& snapshot,
                   const protocol::WorldRules& authoritative_rules);

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

// Authority receipts form a sequenced delivery stream independent of the latest replicated World.
// Accepted bytes, local publication, and server-confirmed retirement advance separately.
class AuthorityReceiptInbox {
public:
    [[nodiscard]] AuthorityReceiptApplyResult apply(const protocol::FullSnapshot& snapshot);
    [[nodiscard]] bool
    mark_published_through(protocol::AuthorityReceiptSequenceId sequence_id) noexcept;
    [[nodiscard]] std::vector<protocol::AuthorityReceipt> pending_publication() const;
    [[nodiscard]] protocol::AuthorityReceiptSequenceId accepted_through() const noexcept;
    [[nodiscard]] protocol::AuthorityReceiptSequenceId published_through() const noexcept;
    [[nodiscard]] protocol::AuthorityReceiptSequenceId server_retired_through() const noexcept;
    [[nodiscard]] bool contains_event_key(const simulation::SimulationEventKey& key) const noexcept;
    [[nodiscard]] std::size_t retained_count() const noexcept;
    [[nodiscard]] std::size_t pending_publication_count() const noexcept;

private:
    std::deque<protocol::AuthorityReceipt> retained_;
    std::map<simulation::SimulationEventKey, protocol::AuthorityReceiptSequenceId> event_keys_;
    protocol::AuthorityReceiptSequenceId accepted_through_;
    protocol::AuthorityReceiptSequenceId published_through_;
    protocol::AuthorityReceiptSequenceId server_retired_through_;
};

} // namespace dots::replication

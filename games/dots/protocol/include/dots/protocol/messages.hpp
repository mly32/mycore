#pragma once

#include "dots/protocol/ids.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace dots::protocol {

enum class MessageKind : std::uint8_t {
    ClientHello = 1,
    ServerWelcome = 2,
    InputPacket = 3,
    FullSnapshot = 4,
};

inline constexpr std::size_t kMaximumInputSamplesPerPacket = 3;
inline constexpr std::uint8_t kMaximumPendingInputCount = 64;
inline constexpr std::size_t kMaximumAuthorityReceiptsPerSnapshot = 16;
inline constexpr std::size_t kMaximumPendingAuthorityReceipts = 256;
inline constexpr std::uint16_t kCheckpointSchemaId = 1;
inline constexpr std::uint16_t kRespawnActionBit = 1U << 0U;
inline constexpr std::uint16_t kSplitActionBit = 1U << 1U;
inline constexpr std::uint16_t kKnownInputActionBits = kRespawnActionBit | kSplitActionBit;

enum class EntityKind : std::uint8_t {
    Player = 1,
    Food = 2,
};

enum class SessionMode : std::uint8_t {
    Playing = 1,
    Spectating = 2,
};

enum class RespawnResult : std::uint8_t {
    None = 0,
    Accepted = 1,
    RejectedCooldown = 2,
    RejectedNotSpectating = 3,
    RejectedNoSafeSpawn = 4,
};

struct ClientHello {
    auto operator<=>(const ClientHello&) const = default;
};

struct WorldRules {
    float initial_player_mass{};
    float food_mass{};
    float spatial_grid_cell_size{};
    float player_speed_units_per_second{};
    std::uint32_t split_recast_ticks{};
    std::uint32_t merge_delay_ticks{};
    std::uint16_t maximum_pieces_per_owner{};
    float minimum_split_mass{};
    float child_launch_speed_units_per_second{};
    float launch_decay_units_per_second_squared{};
    float cohesion_speed_units_per_second{};

    bool operator==(const WorldRules&) const = default;
};

struct ServerWelcome {
    ClientId client_id;
    std::uint32_t server_tick{};
    std::uint32_t respawn_cooldown_ticks{};
    WorldRules world_rules;

    bool operator==(const ServerWelcome&) const = default;
};

struct InputSample {
    InputSequenceId sequence_id;
    std::uint32_t client_tick{};
    float movement_x{};
    float movement_y{};
    std::uint16_t action_bits{};

    auto operator<=>(const InputSample&) const = default;
};

struct InputPacket {
    SnapshotId last_received_snapshot_id;
    AuthorityReceiptSequenceId last_received_authority_receipt_sequence;
    std::vector<InputSample> samples;

    auto operator<=>(const InputPacket&) const = default;
};

struct PredictionKey {
    PlayerOwnerId owner_id;
    InputSequenceId input_id;
    std::uint16_t child_ordinal{};

    auto operator<=>(const PredictionKey&) const = default;
};

struct OwnerState {
    PlayerOwnerId owner_id;
    float movement_x{};
    float movement_y{};
    float last_non_zero_movement_x{};
    float last_non_zero_movement_y{};
    InputSequenceId last_input_id;
    std::uint32_t split_cooldown_end_tick{};

    bool operator==(const OwnerState&) const = default;
};

struct EntityState {
    EntityId entity_id;
    EntityKind kind{EntityKind::Player};
    PlayerOwnerId owner_id;
    float position_x{};
    float position_y{};
    float mass{};
    float launch_velocity_x{};
    float launch_velocity_y{};
    std::uint32_t merge_eligible_tick{};
    std::optional<PredictionKey> prediction_key;

    bool operator==(const EntityState&) const = default;
};

struct FoodConsumed {
    std::uint32_t server_tick{};
    EntityId food_entity_id;
    EntityId consumer_entity_id;
    PlayerOwnerId consumer_owner_id;
    float food_position_x{};
    float food_position_y{};
    float transferred_mass{};

    bool operator==(const FoodConsumed&) const = default;
};

struct PlayerAbsorbed {
    std::uint32_t server_tick{};
    EntityId absorber_entity_id;
    EntityId victim_entity_id;
    PlayerOwnerId absorber_owner_id;
    PlayerOwnerId victim_owner_id;
    float absorber_position_x{};
    float absorber_position_y{};
    float victim_position_x{};
    float victim_position_y{};
    float transferred_mass{};

    auto operator<=>(const PlayerAbsorbed&) const = default;
};

struct PlayerSplit {
    std::uint32_t server_tick{};
    PlayerOwnerId owner_id;
    InputSequenceId input_id;
    std::uint16_t child_ordinal{};
    EntityId parent_entity_id;
    EntityId child_entity_id;
    float origin_position_x{};
    float origin_position_y{};
    float initial_launch_velocity_x{};
    float initial_launch_velocity_y{};
    float parent_mass{};
    float child_mass{};

    bool operator==(const PlayerSplit&) const = default;
};

struct PiecesMerged {
    std::uint32_t server_tick{};
    PlayerOwnerId owner_id;
    EntityId survivor_entity_id;
    EntityId consumed_entity_id;
    float combined_mass{};

    bool operator==(const PiecesMerged&) const = default;
};

using AuthorityEvent = std::variant<FoodConsumed, PlayerAbsorbed, PlayerSplit, PiecesMerged>;

struct AuthorityReceipt {
    AuthorityReceiptSequenceId sequence_id;
    AuthorityEvent event;

    bool operator==(const AuthorityReceipt&) const = default;
};

struct RecipientSessionState {
    SessionMode mode{SessionMode::Playing};
    std::vector<EntityId> owned_entity_ids;
    EntityId primary_entity_id;
    EntityId follow_entity_id;
    std::optional<std::uint32_t> defeat_tick;
    std::optional<std::uint32_t> respawn_available_tick;
    std::optional<PlayerAbsorbed> latest_absorption;
    InputSequenceId latest_respawn_request_id;
    RespawnResult latest_respawn_result{RespawnResult::None};

    auto operator<=>(const RecipientSessionState&) const = default;
};

struct FullSnapshot {
    SnapshotId snapshot_id;
    std::uint32_t server_tick{};
    InputSequenceId last_processed_input_id;
    std::uint8_t pending_input_count{};
    std::uint16_t checkpoint_schema_id{kCheckpointSchemaId};
    std::uint64_t checkpoint_digest{};
    EntityId next_entity_id;
    RecipientSessionState recipient;
    std::vector<OwnerState> owners;
    std::vector<EntityState> entities;
    AuthorityReceiptSequenceId authority_receipts_retired_through;
    std::vector<AuthorityReceipt> authority_receipts;

    bool operator==(const FullSnapshot&) const = default;
};

using Message = std::variant<ClientHello, ServerWelcome, InputPacket, FullSnapshot>;

} // namespace dots::protocol

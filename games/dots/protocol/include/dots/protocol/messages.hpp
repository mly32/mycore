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
inline constexpr std::uint16_t kRespawnActionBit = 1U << 0U;
inline constexpr std::uint16_t kKnownInputActionBits = kRespawnActionBit;

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

struct ServerWelcome {
    ClientId client_id;
    std::uint32_t server_tick{};
    std::uint32_t respawn_cooldown_ticks{};

    auto operator<=>(const ServerWelcome&) const = default;
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
    std::vector<InputSample> samples;

    auto operator<=>(const InputPacket&) const = default;
};

struct EntityState {
    EntityId entity_id;
    EntityKind kind{EntityKind::Player};
    PlayerOwnerId owner_id;
    float position_x{};
    float position_y{};
    float mass{};

    auto operator<=>(const EntityState&) const = default;
};

struct PlayerAbsorbed {
    std::uint32_t server_tick{};
    EntityId absorber_entity_id;
    EntityId victim_entity_id;
    PlayerOwnerId absorber_owner_id;
    PlayerOwnerId victim_owner_id;
    float transferred_mass{};

    auto operator<=>(const PlayerAbsorbed&) const = default;
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
    RecipientSessionState recipient;
    std::vector<EntityState> entities;

    auto operator<=>(const FullSnapshot&) const = default;
};

using Message = std::variant<ClientHello, ServerWelcome, InputPacket, FullSnapshot>;

} // namespace dots::protocol

#pragma once

#include "dots/protocol/ids.hpp"

#include <compare>
#include <cstdint>
#include <variant>
#include <vector>

namespace dots::protocol {

enum class MessageKind : std::uint8_t {
    ClientHello = 1,
    ServerWelcome = 2,
    InputCommand = 3,
    FullSnapshot = 4,
};

enum class EntityKind : std::uint8_t {
    Player = 1,
    Food = 2,
};

struct ClientHello {
    auto operator<=>(const ClientHello&) const = default;
};

struct ServerWelcome {
    ClientId client_id;
    EntityId controlled_entity_id;
    std::uint32_t server_tick{};

    auto operator<=>(const ServerWelcome&) const = default;
};

struct InputCommand {
    InputSequenceId sequence_id;
    std::uint32_t client_tick{};
    float movement_x{};
    float movement_y{};
    std::uint16_t action_bits{};
    SnapshotId last_received_snapshot_id;

    auto operator<=>(const InputCommand&) const = default;
};

struct EntityState {
    EntityId entity_id;
    EntityKind kind{EntityKind::Player};
    float position_x{};
    float position_y{};
    float mass{};

    auto operator<=>(const EntityState&) const = default;
};

struct FullSnapshot {
    SnapshotId snapshot_id;
    std::uint32_t server_tick{};
    InputSequenceId last_processed_input_id;
    std::vector<EntityState> entities;

    auto operator<=>(const FullSnapshot&) const = default;
};

using Message = std::variant<ClientHello, ServerWelcome, InputCommand, FullSnapshot>;

} // namespace dots::protocol

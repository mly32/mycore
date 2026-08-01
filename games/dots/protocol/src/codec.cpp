#include "dots/protocol/codec.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace dots::protocol {
namespace {

constexpr std::uint8_t kMagicD = 0x44;
constexpr std::uint8_t kMagicO = 0x4F;
constexpr std::uint8_t kMagicT = 0x54;
constexpr std::uint8_t kMagicS = 0x53;
constexpr std::uint8_t kSupportedFlags = 0;
constexpr std::size_t kWorldRulesBytes = 42;
constexpr std::size_t kClientHelloPayloadBytes = 1;
constexpr std::size_t kServerWelcomePayloadBytes = 13 + kWorldRulesBytes;
constexpr std::size_t kClientStatusPayloadBytes = 8;
constexpr std::size_t kInputPacketPrefixBytes = 9;
constexpr std::size_t kInputSampleBytes = 18;
constexpr std::size_t kFullSnapshotBaseBytes = 61;
constexpr std::size_t kOwnedEntityIdBytes = 4;
constexpr std::size_t kPlayerAbsorbedBytes = 40;
constexpr std::size_t kOwnerStateBytes = 28;
constexpr std::size_t kEntityStateBaseBytes = 34;
constexpr std::size_t kPredictionKeyBytes = 10;
constexpr std::size_t kAuthorityReceiptPrefixBytes = 5;
constexpr std::size_t kFoodConsumedBytes = 28;
constexpr std::size_t kPlayerSplitBytes = 46;
constexpr std::size_t kPiecesMergedBytes = 20;
constexpr float kMaximumMovementLengthSquared = 1.0001F;

enum class AuthorityEventKind : std::uint8_t {
    FoodConsumed = 1,
    PlayerAbsorbed = 2,
    PlayerSplit = 3,
    PiecesMerged = 4,
};

static_assert(sizeof(float) == sizeof(std::uint32_t));
static_assert(std::numeric_limits<float>::is_iec559);
static_assert(kPacketHeaderBytes + kInputPacketPrefixBytes +
                  (kMaximumInputSamplesPerPacket * kInputSampleBytes) ==
              kMaximumEncodedInputPacketBytes);

class Writer {
public:
    explicit Writer(std::size_t capacity = 0) {
        bytes_.reserve(capacity);
    }

    void write_u8(std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void write_u16(std::uint16_t value) {
        write_u8(static_cast<std::uint8_t>(value >> 8U));
        write_u8(static_cast<std::uint8_t>(value));
    }

    void write_u32(std::uint32_t value) {
        write_u8(static_cast<std::uint8_t>(value >> 24U));
        write_u8(static_cast<std::uint8_t>(value >> 16U));
        write_u8(static_cast<std::uint8_t>(value >> 8U));
        write_u8(static_cast<std::uint8_t>(value));
    }

    void write_u64(std::uint64_t value) {
        write_u32(static_cast<std::uint32_t>(value >> 32U));
        write_u32(static_cast<std::uint32_t>(value));
    }

    void write_float(float value) {
        write_u32(std::bit_cast<std::uint32_t>(value));
    }

    void append(std::span<const std::byte> bytes) {
        bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    }

    [[nodiscard]] EncodedMessage take_bytes() {
        return std::move(bytes_);
    }

private:
    EncodedMessage bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes)
        : bytes_(bytes) {}

    [[nodiscard]] bool read_u8(std::uint8_t& value) {
        if (remaining() < 1) {
            return false;
        }
        value = std::to_integer<std::uint8_t>(bytes_[offset_]);
        ++offset_;
        return true;
    }

    [[nodiscard]] bool read_u16(std::uint16_t& value) {
        std::uint8_t high{};
        std::uint8_t low{};
        if (!read_u8(high) || !read_u8(low)) {
            return false;
        }
        value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 8U) |
                                           static_cast<std::uint16_t>(low));
        return true;
    }

    [[nodiscard]] bool read_u32(std::uint32_t& value) {
        std::uint8_t byte_0{};
        std::uint8_t byte_1{};
        std::uint8_t byte_2{};
        std::uint8_t byte_3{};
        if (!read_u8(byte_0) || !read_u8(byte_1) || !read_u8(byte_2) || !read_u8(byte_3)) {
            return false;
        }
        value = (static_cast<std::uint32_t>(byte_0) << 24U) |
                (static_cast<std::uint32_t>(byte_1) << 16U) |
                (static_cast<std::uint32_t>(byte_2) << 8U) | static_cast<std::uint32_t>(byte_3);
        return true;
    }

    [[nodiscard]] bool read_u64(std::uint64_t& value) {
        std::uint32_t high{};
        std::uint32_t low{};
        if (!read_u32(high) || !read_u32(low)) {
            return false;
        }
        value = (static_cast<std::uint64_t>(high) << 32U) | static_cast<std::uint64_t>(low);
        return true;
    }

    [[nodiscard]] bool read_float(float& value) {
        std::uint32_t bits{};
        if (!read_u32(bits)) {
            return false;
        }
        value = std::bit_cast<float>(bits);
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

[[nodiscard]] bool is_known(EntityKind kind) noexcept {
    switch (kind) {
    case EntityKind::Player:
    case EntityKind::Food:
        return true;
    }
    return false;
}

[[nodiscard]] bool is_known(SessionMode mode) noexcept {
    switch (mode) {
    case SessionMode::Playing:
    case SessionMode::Spectating:
        return true;
    }
    return false;
}

[[nodiscard]] bool is_known(JoinRole role) noexcept {
    switch (role) {
    case JoinRole::Player:
    case JoinRole::Spectator:
        return true;
    }
    return false;
}

[[nodiscard]] bool is_known(RespawnResult result) noexcept {
    switch (result) {
    case RespawnResult::None:
    case RespawnResult::Accepted:
    case RespawnResult::RejectedCooldown:
    case RespawnResult::RejectedNotSpectating:
    case RespawnResult::RejectedNoSafeSpawn:
        return true;
    }
    return false;
}

[[nodiscard]] bool is_known(AuthorityEventKind kind) noexcept {
    switch (kind) {
    case AuthorityEventKind::FoodConsumed:
    case AuthorityEventKind::PlayerAbsorbed:
    case AuthorityEventKind::PlayerSplit:
    case AuthorityEventKind::PiecesMerged:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_movement(float x, float y) noexcept {
    return std::isfinite(x) && std::isfinite(y) && x >= -1.0F && x <= 1.0F && y >= -1.0F &&
           y <= 1.0F && ((x * x) + (y * y)) <= kMaximumMovementLengthSquared;
}

[[nodiscard]] bool valid_rules(const WorldRules& rules) noexcept {
    return std::isfinite(rules.initial_player_mass) && rules.initial_player_mass > 0.0F &&
           std::isfinite(rules.food_mass) && rules.food_mass > 0.0F &&
           std::isfinite(rules.spatial_grid_cell_size) && rules.spatial_grid_cell_size > 0.0F &&
           std::isfinite(rules.player_speed_units_per_second) &&
           rules.player_speed_units_per_second > 0.0F && rules.split_recast_ticks > 0 &&
           rules.merge_delay_ticks > 0 && rules.maximum_pieces_per_owner > 0 &&
           std::isfinite(rules.minimum_split_mass) && rules.minimum_split_mass > 0.0F &&
           std::isfinite(rules.child_launch_speed_units_per_second) &&
           rules.child_launch_speed_units_per_second >= 0.0F &&
           std::isfinite(rules.launch_decay_units_per_second_squared) &&
           rules.launch_decay_units_per_second_squared >= 0.0F &&
           std::isfinite(rules.cohesion_speed_units_per_second) &&
           rules.cohesion_speed_units_per_second >= 0.0F;
}

template <class Values, class Projection>
[[nodiscard]] bool is_strictly_sorted(const Values& values, Projection projection) {
    return std::adjacent_find(values.begin(), values.end(), [&](const auto& lhs, const auto& rhs) {
               return projection(lhs) >= projection(rhs);
           }) == values.end();
}

[[nodiscard]] std::optional<CodecError> validate(const ClientHello& message) noexcept {
    return is_known(message.requested_role) ? std::nullopt : std::optional{CodecError::InvalidEnum};
}

[[nodiscard]] std::optional<CodecError> validate(const ServerWelcome& message) noexcept {
    if (!message.client_id.is_valid() || !is_known(message.accepted_role)) {
        return !message.client_id.is_valid() ? CodecError::InvalidId : CodecError::InvalidEnum;
    }
    if (!valid_rules(message.world_rules)) {
        return CodecError::InvalidCheckpoint;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CodecError> validate(const ClientStatus& message) noexcept {
    if (!message.last_received_snapshot_id.is_valid()) {
        return CodecError::InvalidId;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CodecError> validate(const InputSample& sample) noexcept {
    if (!sample.sequence_id.is_valid()) {
        return CodecError::InvalidId;
    }
    if ((sample.action_bits & static_cast<std::uint16_t>(~kKnownInputActionBits)) != 0) {
        return CodecError::OutOfRange;
    }
    if (!std::isfinite(sample.movement_x) || !std::isfinite(sample.movement_y)) {
        return CodecError::InvalidNumber;
    }
    if (!valid_movement(sample.movement_x, sample.movement_y)) {
        return CodecError::OutOfRange;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CodecError> validate(const InputPacket& message) noexcept {
    if (!message.last_received_snapshot_id.is_valid()) {
        return CodecError::InvalidId;
    }
    if (message.samples.empty() || message.samples.size() > kMaximumInputSamplesPerPacket) {
        return CodecError::OutOfRange;
    }
    for (std::size_t index = 0; index < message.samples.size(); ++index) {
        const auto& sample = message.samples[index];
        if (const auto error = validate(sample)) {
            return error;
        }
        if (index == 0) {
            continue;
        }
        const auto& previous = message.samples[index - 1];
        if (previous.sequence_id.value() == std::numeric_limits<std::uint32_t>::max() - 1U ||
            sample.sequence_id.value() != previous.sequence_id.value() + 1U ||
            previous.client_tick == std::numeric_limits<std::uint32_t>::max() ||
            sample.client_tick != previous.client_tick + 1U) {
            return CodecError::InvalidInputOrdering;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CodecError> validate_absorption(const PlayerAbsorbed& event,
                                                            std::uint32_t snapshot_tick) noexcept {
    if (!event.absorber_entity_id.is_valid() || !event.victim_entity_id.is_valid() ||
        !event.absorber_owner_id.is_valid() || !event.victim_owner_id.is_valid() ||
        event.absorber_entity_id == event.victim_entity_id ||
        event.absorber_owner_id == event.victim_owner_id) {
        return CodecError::InvalidId;
    }
    if (!std::isfinite(event.absorber_position_x) || !std::isfinite(event.absorber_position_y) ||
        !std::isfinite(event.victim_position_x) || !std::isfinite(event.victim_position_y) ||
        !std::isfinite(event.transferred_mass)) {
        return CodecError::InvalidNumber;
    }
    if (event.transferred_mass <= 0.0F || event.server_tick > snapshot_tick) {
        return CodecError::OutOfRange;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CodecError> validate_event(const AuthorityEvent& value,
                                                       std::uint32_t snapshot_tick) {
    return std::visit(
        [snapshot_tick](const auto& event) -> std::optional<CodecError> {
            using Event = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<Event, FoodConsumed>) {
                if (!event.food_entity_id.is_valid() || !event.consumer_entity_id.is_valid() ||
                    !event.consumer_owner_id.is_valid() ||
                    event.food_entity_id == event.consumer_entity_id) {
                    return CodecError::InvalidId;
                }
                if (!std::isfinite(event.food_position_x) ||
                    !std::isfinite(event.food_position_y) ||
                    !std::isfinite(event.transferred_mass)) {
                    return CodecError::InvalidNumber;
                }
                if (event.transferred_mass <= 0.0F || event.server_tick > snapshot_tick) {
                    return CodecError::OutOfRange;
                }
            } else if constexpr (std::is_same_v<Event, PlayerAbsorbed>) {
                return validate_absorption(event, snapshot_tick);
            } else if constexpr (std::is_same_v<Event, PlayerSplit>) {
                if (!event.owner_id.is_valid() || !event.input_id.is_valid() ||
                    !event.parent_entity_id.is_valid() || !event.child_entity_id.is_valid() ||
                    event.parent_entity_id == event.child_entity_id) {
                    return CodecError::InvalidId;
                }
                if (!std::isfinite(event.origin_position_x) ||
                    !std::isfinite(event.origin_position_y) ||
                    !std::isfinite(event.initial_launch_velocity_x) ||
                    !std::isfinite(event.initial_launch_velocity_y) ||
                    !std::isfinite(event.parent_mass) || !std::isfinite(event.child_mass)) {
                    return CodecError::InvalidNumber;
                }
                if (event.parent_mass <= 0.0F || event.child_mass <= 0.0F ||
                    event.server_tick > snapshot_tick) {
                    return CodecError::OutOfRange;
                }
            } else {
                if (!event.owner_id.is_valid() || !event.survivor_entity_id.is_valid() ||
                    !event.consumed_entity_id.is_valid() ||
                    event.survivor_entity_id == event.consumed_entity_id) {
                    return CodecError::InvalidId;
                }
                if (!std::isfinite(event.combined_mass)) {
                    return CodecError::InvalidNumber;
                }
                if (event.combined_mass <= 0.0F || event.server_tick > snapshot_tick) {
                    return CodecError::OutOfRange;
                }
            }
            return std::nullopt;
        },
        value);
}

[[nodiscard]] std::optional<CodecError> validate(const FullSnapshot& message) {
    if (!message.snapshot_id.is_valid()) {
        return CodecError::InvalidId;
    }
    if (message.pending_input_count > kMaximumPendingInputCount) {
        return CodecError::OutOfRange;
    }
    if (message.input_receive_through.is_valid()) {
        if (message.input_receive_through !=
                input_receive_through_for(message.last_processed_input_id) ||
            message.pending_input_count > kInputReceiveWindow) {
            return CodecError::InvalidInputOrdering;
        }
    } else if (message.pending_input_count != 0) {
        return CodecError::InvalidInputOrdering;
    }
    if (message.checkpoint_schema_id != kCheckpointSchemaId) {
        return CodecError::InvalidCheckpoint;
    }
    if (message.entities.size() > std::numeric_limits<std::uint16_t>::max() ||
        message.recipient.owned_entity_ids.size() > std::numeric_limits<std::uint16_t>::max() ||
        message.owners.size() > std::numeric_limits<std::uint16_t>::max()) {
        return CodecError::TooManyEntities;
    }
    if (message.authority_receipts.size() > kMaximumAuthorityReceiptsPerSnapshot) {
        return CodecError::TooManyReceipts;
    }
    if (!is_known(message.recipient.mode) || !is_known(message.recipient.latest_respawn_result)) {
        return CodecError::InvalidEnum;
    }
    const auto invalid_optional_tick = [](const std::optional<std::uint32_t>& tick) {
        return tick && *tick == std::numeric_limits<std::uint32_t>::max();
    };
    if (invalid_optional_tick(message.recipient.defeat_tick) ||
        invalid_optional_tick(message.recipient.respawn_available_tick)) {
        return CodecError::OutOfRange;
    }
    const auto has_respawn_request = message.recipient.latest_respawn_request_id.is_valid();
    if (has_respawn_request != (message.recipient.latest_respawn_result != RespawnResult::None)) {
        return CodecError::InvalidId;
    }
    if (has_respawn_request &&
        (!message.last_processed_input_id.is_valid() ||
         message.recipient.latest_respawn_request_id > message.last_processed_input_id)) {
        return CodecError::InvalidInputOrdering;
    }
    if (message.recipient.latest_absorption) {
        if (const auto error =
                validate_absorption(*message.recipient.latest_absorption, message.server_tick)) {
            return error;
        }
    }

    if (!is_strictly_sorted(message.owners, [](const OwnerState& owner) {
            return owner.owner_id;
        })) {
        return CodecError::DuplicateOwner;
    }
    std::unordered_map<std::uint32_t, const OwnerState*> owners_by_id;
    std::unordered_map<std::uint32_t, bool> owner_has_piece;
    owners_by_id.reserve(message.owners.size());
    owner_has_piece.reserve(message.owners.size());
    for (const auto& owner : message.owners) {
        if (!owner.owner_id.is_valid()) {
            return CodecError::InvalidId;
        }
        if (!valid_movement(owner.movement_x, owner.movement_y) ||
            !valid_movement(owner.last_non_zero_movement_x, owner.last_non_zero_movement_y)) {
            return CodecError::OutOfRange;
        }
        owners_by_id.emplace(owner.owner_id.value(), &owner);
        owner_has_piece.emplace(owner.owner_id.value(), false);
    }

    if (!is_strictly_sorted(message.entities, [](const EntityState& entity) {
            return entity.entity_id;
        })) {
        return CodecError::DuplicateEntity;
    }
    std::unordered_map<std::uint32_t, const EntityState*> entities_by_id;
    std::set<PredictionKey> prediction_keys;
    entities_by_id.reserve(message.entities.size());
    for (const auto& entity : message.entities) {
        if (!entity.entity_id.is_valid()) {
            return CodecError::InvalidId;
        }
        if (message.next_entity_id.is_valid() && entity.entity_id >= message.next_entity_id) {
            return CodecError::InvalidCheckpoint;
        }
        if (!is_known(entity.kind)) {
            return CodecError::InvalidEnum;
        }
        if (!std::isfinite(entity.position_x) || !std::isfinite(entity.position_y) ||
            !std::isfinite(entity.mass) || !std::isfinite(entity.launch_velocity_x) ||
            !std::isfinite(entity.launch_velocity_y)) {
            return CodecError::InvalidNumber;
        }
        if (entity.mass <= 0.0F) {
            return CodecError::OutOfRange;
        }
        if (entity.kind == EntityKind::Player) {
            const auto owner = owners_by_id.find(entity.owner_id.value());
            if (!entity.owner_id.is_valid() || owner == owners_by_id.end()) {
                return CodecError::InvalidId;
            }
            owner_has_piece.at(entity.owner_id.value()) = true;
            if (entity.prediction_key) {
                if (!entity.prediction_key->owner_id.is_valid() ||
                    !entity.prediction_key->input_id.is_valid() ||
                    entity.prediction_key->owner_id != entity.owner_id ||
                    !owner->second->last_input_id.is_valid() ||
                    entity.prediction_key->input_id > owner->second->last_input_id ||
                    !prediction_keys.insert(*entity.prediction_key).second) {
                    return CodecError::InvalidCheckpoint;
                }
            }
        } else if (entity.owner_id.is_valid() || entity.launch_velocity_x != 0.0F ||
                   entity.launch_velocity_y != 0.0F || entity.merge_eligible_tick != 0 ||
                   entity.prediction_key) {
            return CodecError::InvalidCheckpoint;
        }
        entities_by_id.emplace(entity.entity_id.value(), &entity);
    }
    if (std::ranges::any_of(owner_has_piece, [](const auto& entry) {
            return !entry.second;
        })) {
        return CodecError::InvalidCheckpoint;
    }

    if (!is_strictly_sorted(message.recipient.owned_entity_ids, [](EntityId entity_id) {
            return entity_id;
        })) {
        return CodecError::DuplicateEntity;
    }
    PlayerOwnerId owned_owner;
    std::unordered_set<std::uint32_t> owned_ids;
    owned_ids.reserve(message.recipient.owned_entity_ids.size());
    for (const auto entity_id : message.recipient.owned_entity_ids) {
        if (!entity_id.is_valid()) {
            return CodecError::InvalidId;
        }
        const auto entity = entities_by_id.find(entity_id.value());
        if (entity == entities_by_id.end() || entity->second->kind != EntityKind::Player) {
            return CodecError::InvalidId;
        }
        if (owned_owner.is_valid() && entity->second->owner_id != owned_owner) {
            return CodecError::InvalidId;
        }
        owned_owner = entity->second->owner_id;
        owned_ids.insert(entity_id.value());
    }
    if (message.recipient.follow_entity_id.is_valid()) {
        const auto follow = entities_by_id.find(message.recipient.follow_entity_id.value());
        if (follow == entities_by_id.end() || follow->second->kind != EntityKind::Player) {
            return CodecError::InvalidId;
        }
    }
    if (message.recipient.latest_absorption) {
        const auto& event = *message.recipient.latest_absorption;
        const auto absorber = entities_by_id.find(event.absorber_entity_id.value());
        if (absorber != entities_by_id.end() &&
            (absorber->second->kind != EntityKind::Player ||
             absorber->second->owner_id != event.absorber_owner_id)) {
            return CodecError::InvalidId;
        }
        if (entities_by_id.contains(event.victim_entity_id.value())) {
            return CodecError::InvalidId;
        }
    }

    if (message.recipient.mode == SessionMode::Playing) {
        if (message.recipient.owned_entity_ids.empty() ||
            !message.recipient.primary_entity_id.is_valid() ||
            !owned_ids.contains(message.recipient.primary_entity_id.value()) ||
            message.recipient.follow_entity_id.is_valid() || message.recipient.defeat_tick ||
            message.recipient.respawn_available_tick) {
            return CodecError::InvalidId;
        }
    } else {
        if (!message.recipient.owned_entity_ids.empty() ||
            message.recipient.primary_entity_id.is_valid() ||
            message.recipient.defeat_tick.has_value() !=
                message.recipient.respawn_available_tick.has_value()) {
            return CodecError::InvalidId;
        }
        if (message.recipient.defeat_tick &&
            (*message.recipient.defeat_tick > message.server_tick ||
             *message.recipient.respawn_available_tick < *message.recipient.defeat_tick)) {
            return CodecError::OutOfRange;
        }
        if (!message.recipient.defeat_tick &&
            (message.recipient.follow_entity_id.is_valid() ||
             message.recipient.latest_respawn_request_id.is_valid() ||
             message.recipient.latest_respawn_result != RespawnResult::None)) {
            return CodecError::InvalidId;
        }
    }

    std::set<std::pair<std::uint8_t, std::vector<std::uint32_t>>> event_keys;
    if (!message.authority_receipts.empty()) {
        const auto expected_first = message.authority_receipts_retired_through.is_valid()
                                        ? message.authority_receipts_retired_through.value() + 1U
                                        : std::uint32_t{0};
        if (message.authority_receipts_retired_through.is_valid() &&
            message.authority_receipts_retired_through.value() ==
                AuthorityReceiptSequenceId::kInvalidValue - 1U) {
            return CodecError::InvalidReceiptOrdering;
        }
        if (message.authority_receipts.front().sequence_id.value() != expected_first) {
            return CodecError::InvalidReceiptOrdering;
        }
    }
    for (std::size_t index = 0; index < message.authority_receipts.size(); ++index) {
        const auto& receipt = message.authority_receipts[index];
        if (!receipt.sequence_id.is_valid()) {
            return CodecError::InvalidId;
        }
        if (index > 0) {
            const auto previous = message.authority_receipts[index - 1].sequence_id.value();
            if (previous == AuthorityReceiptSequenceId::kInvalidValue - 1U ||
                receipt.sequence_id.value() != previous + 1U) {
                return CodecError::InvalidReceiptOrdering;
            }
        }
        if (const auto error = validate_event(receipt.event, message.server_tick)) {
            return error;
        }
        const auto key = std::visit(
            [](const auto& event) {
                using Event = std::decay_t<decltype(event)>;
                if constexpr (std::is_same_v<Event, FoodConsumed>) {
                    return std::pair{std::uint8_t{1},
                                     std::vector<std::uint32_t>{event.food_entity_id.value()}};
                } else if constexpr (std::is_same_v<Event, PlayerAbsorbed>) {
                    return std::pair{std::uint8_t{2},
                                     std::vector<std::uint32_t>{event.victim_entity_id.value()}};
                } else if constexpr (std::is_same_v<Event, PlayerSplit>) {
                    return std::pair{std::uint8_t{3},
                                     std::vector<std::uint32_t>{event.owner_id.value(),
                                                                event.input_id.value(),
                                                                event.child_ordinal}};
                } else {
                    const auto first = std::min(event.survivor_entity_id, event.consumed_entity_id);
                    const auto second =
                        std::max(event.survivor_entity_id, event.consumed_entity_id);
                    return std::pair{std::uint8_t{4},
                                     std::vector<std::uint32_t>{first.value(), second.value()}};
                }
            },
            receipt.event);
        if (!event_keys.insert(key).second) {
            return CodecError::InvalidCheckpoint;
        }
    }
    return std::nullopt;
}

[[nodiscard]] MessageKind message_kind(const Message& message) {
    return std::visit(
        [](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, ClientHello>) {
                return MessageKind::ClientHello;
            } else if constexpr (std::is_same_v<Value, ServerWelcome>) {
                return MessageKind::ServerWelcome;
            } else if constexpr (std::is_same_v<Value, InputPacket>) {
                return MessageKind::InputPacket;
            } else if constexpr (std::is_same_v<Value, FullSnapshot>) {
                return MessageKind::FullSnapshot;
            } else {
                return MessageKind::ClientStatus;
            }
        },
        message);
}

[[nodiscard]] std::optional<CodecError> validate_message(const Message& message) {
    return std::visit(
        [](const auto& value) {
            return validate(value);
        },
        message);
}

[[nodiscard]] std::size_t authority_event_size(const AuthorityEvent& event) {
    return std::visit(
        [](const auto& value) -> std::size_t {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, FoodConsumed>) {
                return kFoodConsumedBytes;
            } else if constexpr (std::is_same_v<Event, PlayerAbsorbed>) {
                return kPlayerAbsorbedBytes;
            } else if constexpr (std::is_same_v<Event, PlayerSplit>) {
                return kPlayerSplitBytes;
            } else {
                return kPiecesMergedBytes;
            }
        },
        event);
}

[[nodiscard]] std::size_t payload_size(const Message& message) {
    return std::visit(
        [](const auto& value) -> std::size_t {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, ClientHello>) {
                return kClientHelloPayloadBytes;
            } else if constexpr (std::is_same_v<Value, ServerWelcome>) {
                return kServerWelcomePayloadBytes;
            } else if constexpr (std::is_same_v<Value, InputPacket>) {
                return kInputPacketPrefixBytes + (value.samples.size() * kInputSampleBytes);
            } else if constexpr (std::is_same_v<Value, ClientStatus>) {
                return kClientStatusPayloadBytes;
            } else {
                auto size = kFullSnapshotBaseBytes +
                            (value.recipient.owned_entity_ids.size() * kOwnedEntityIdBytes) +
                            (value.recipient.latest_absorption ? kPlayerAbsorbedBytes : 0U) +
                            (value.owners.size() * kOwnerStateBytes);
                for (const auto& entity : value.entities) {
                    size +=
                        kEntityStateBaseBytes + (entity.prediction_key ? kPredictionKeyBytes : 0U);
                }
                for (const auto& receipt : value.authority_receipts) {
                    size += kAuthorityReceiptPrefixBytes + authority_event_size(receipt.event);
                }
                return size;
            }
        },
        message);
}

void encode_rules(Writer& writer, const WorldRules& rules) {
    writer.write_float(rules.initial_player_mass);
    writer.write_float(rules.food_mass);
    writer.write_float(rules.spatial_grid_cell_size);
    writer.write_float(rules.player_speed_units_per_second);
    writer.write_u32(rules.split_recast_ticks);
    writer.write_u32(rules.merge_delay_ticks);
    writer.write_u16(rules.maximum_pieces_per_owner);
    writer.write_float(rules.minimum_split_mass);
    writer.write_float(rules.child_launch_speed_units_per_second);
    writer.write_float(rules.launch_decay_units_per_second_squared);
    writer.write_float(rules.cohesion_speed_units_per_second);
}

[[nodiscard]] bool decode_rules(Reader& reader, WorldRules& rules) {
    return reader.read_float(rules.initial_player_mass) && reader.read_float(rules.food_mass) &&
           reader.read_float(rules.spatial_grid_cell_size) &&
           reader.read_float(rules.player_speed_units_per_second) &&
           reader.read_u32(rules.split_recast_ticks) && reader.read_u32(rules.merge_delay_ticks) &&
           reader.read_u16(rules.maximum_pieces_per_owner) &&
           reader.read_float(rules.minimum_split_mass) &&
           reader.read_float(rules.child_launch_speed_units_per_second) &&
           reader.read_float(rules.launch_decay_units_per_second_squared) &&
           reader.read_float(rules.cohesion_speed_units_per_second);
}

void encode_absorption(Writer& writer, const PlayerAbsorbed& event) {
    writer.write_u32(event.server_tick);
    writer.write_u32(event.absorber_entity_id.value());
    writer.write_u32(event.victim_entity_id.value());
    writer.write_u32(event.absorber_owner_id.value());
    writer.write_u32(event.victim_owner_id.value());
    writer.write_float(event.absorber_position_x);
    writer.write_float(event.absorber_position_y);
    writer.write_float(event.victim_position_x);
    writer.write_float(event.victim_position_y);
    writer.write_float(event.transferred_mass);
}

[[nodiscard]] bool decode_absorption(Reader& reader, PlayerAbsorbed& event) {
    std::uint32_t absorber_entity_id{};
    std::uint32_t victim_entity_id{};
    std::uint32_t absorber_owner_id{};
    std::uint32_t victim_owner_id{};
    if (!reader.read_u32(event.server_tick) || !reader.read_u32(absorber_entity_id) ||
        !reader.read_u32(victim_entity_id) || !reader.read_u32(absorber_owner_id) ||
        !reader.read_u32(victim_owner_id) || !reader.read_float(event.absorber_position_x) ||
        !reader.read_float(event.absorber_position_y) ||
        !reader.read_float(event.victim_position_x) ||
        !reader.read_float(event.victim_position_y) || !reader.read_float(event.transferred_mass)) {
        return false;
    }
    event.absorber_entity_id = EntityId{absorber_entity_id};
    event.victim_entity_id = EntityId{victim_entity_id};
    event.absorber_owner_id = PlayerOwnerId{absorber_owner_id};
    event.victim_owner_id = PlayerOwnerId{victim_owner_id};
    return true;
}

void encode_authority_event(Writer& writer, const AuthorityEvent& event) {
    std::visit(
        [&writer](const auto& value) {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, FoodConsumed>) {
                writer.write_u8(static_cast<std::uint8_t>(AuthorityEventKind::FoodConsumed));
                writer.write_u32(value.server_tick);
                writer.write_u32(value.food_entity_id.value());
                writer.write_u32(value.consumer_entity_id.value());
                writer.write_u32(value.consumer_owner_id.value());
                writer.write_float(value.food_position_x);
                writer.write_float(value.food_position_y);
                writer.write_float(value.transferred_mass);
            } else if constexpr (std::is_same_v<Event, PlayerAbsorbed>) {
                writer.write_u8(static_cast<std::uint8_t>(AuthorityEventKind::PlayerAbsorbed));
                encode_absorption(writer, value);
            } else if constexpr (std::is_same_v<Event, PlayerSplit>) {
                writer.write_u8(static_cast<std::uint8_t>(AuthorityEventKind::PlayerSplit));
                writer.write_u32(value.server_tick);
                writer.write_u32(value.owner_id.value());
                writer.write_u32(value.input_id.value());
                writer.write_u16(value.child_ordinal);
                writer.write_u32(value.parent_entity_id.value());
                writer.write_u32(value.child_entity_id.value());
                writer.write_float(value.origin_position_x);
                writer.write_float(value.origin_position_y);
                writer.write_float(value.initial_launch_velocity_x);
                writer.write_float(value.initial_launch_velocity_y);
                writer.write_float(value.parent_mass);
                writer.write_float(value.child_mass);
            } else {
                writer.write_u8(static_cast<std::uint8_t>(AuthorityEventKind::PiecesMerged));
                writer.write_u32(value.server_tick);
                writer.write_u32(value.owner_id.value());
                writer.write_u32(value.survivor_entity_id.value());
                writer.write_u32(value.consumed_entity_id.value());
                writer.write_float(value.combined_mass);
            }
        },
        event);
}

void encode_payload(Writer& writer, const ClientHello& message) {
    writer.write_u8(static_cast<std::uint8_t>(message.requested_role));
}

void encode_payload(Writer& writer, const ServerWelcome& message) {
    writer.write_u32(message.client_id.value());
    writer.write_u8(static_cast<std::uint8_t>(message.accepted_role));
    writer.write_u32(message.server_tick);
    writer.write_u32(message.respawn_cooldown_ticks);
    encode_rules(writer, message.world_rules);
}

void encode_payload(Writer& writer, const InputPacket& message) {
    writer.write_u8(static_cast<std::uint8_t>(message.samples.size()));
    writer.write_u32(message.last_received_snapshot_id.value());
    writer.write_u32(message.last_received_authority_receipt_sequence.value());
    for (const auto& sample : message.samples) {
        writer.write_u32(sample.sequence_id.value());
        writer.write_u32(sample.client_tick);
        writer.write_float(sample.movement_x);
        writer.write_float(sample.movement_y);
        writer.write_u16(sample.action_bits);
    }
}

void encode_payload(Writer& writer, const ClientStatus& message) {
    writer.write_u32(message.last_received_snapshot_id.value());
    writer.write_u32(message.last_received_authority_receipt_sequence.value());
}

void encode_payload(Writer& writer, const FullSnapshot& message) {
    writer.write_u32(message.snapshot_id.value());
    writer.write_u32(message.server_tick);
    writer.write_u32(message.last_processed_input_id.value());
    writer.write_u32(message.input_receive_through.value());
    writer.write_u8(message.pending_input_count);
    writer.write_u32(message.authority_receipts_retired_through.value());
    writer.write_u16(message.checkpoint_schema_id);
    writer.write_u64(message.checkpoint_digest);
    writer.write_u32(message.next_entity_id.value());
    writer.write_u8(static_cast<std::uint8_t>(message.recipient.mode));
    writer.write_u32(message.recipient.primary_entity_id.value());
    writer.write_u32(message.recipient.follow_entity_id.value());
    writer.write_u32(
        message.recipient.defeat_tick.value_or(std::numeric_limits<std::uint32_t>::max()));
    writer.write_u32(message.recipient.respawn_available_tick.value_or(
        std::numeric_limits<std::uint32_t>::max()));
    writer.write_u32(message.recipient.latest_respawn_request_id.value());
    writer.write_u8(static_cast<std::uint8_t>(message.recipient.latest_respawn_result));
    writer.write_u16(static_cast<std::uint16_t>(message.recipient.owned_entity_ids.size()));
    writer.write_u8(message.recipient.latest_absorption ? 1U : 0U);
    for (const auto entity_id : message.recipient.owned_entity_ids) {
        writer.write_u32(entity_id.value());
    }
    if (message.recipient.latest_absorption) {
        encode_absorption(writer, *message.recipient.latest_absorption);
    }
    writer.write_u16(static_cast<std::uint16_t>(message.owners.size()));
    for (const auto& owner : message.owners) {
        writer.write_u32(owner.owner_id.value());
        writer.write_float(owner.movement_x);
        writer.write_float(owner.movement_y);
        writer.write_float(owner.last_non_zero_movement_x);
        writer.write_float(owner.last_non_zero_movement_y);
        writer.write_u32(owner.last_input_id.value());
        writer.write_u32(owner.split_cooldown_end_tick);
    }
    writer.write_u16(static_cast<std::uint16_t>(message.entities.size()));
    for (const auto& entity : message.entities) {
        writer.write_u32(entity.entity_id.value());
        writer.write_u8(static_cast<std::uint8_t>(entity.kind));
        writer.write_u32(entity.owner_id.value());
        writer.write_float(entity.position_x);
        writer.write_float(entity.position_y);
        writer.write_float(entity.mass);
        writer.write_float(entity.launch_velocity_x);
        writer.write_float(entity.launch_velocity_y);
        writer.write_u32(entity.merge_eligible_tick);
        writer.write_u8(entity.prediction_key ? 1U : 0U);
        if (entity.prediction_key) {
            writer.write_u32(entity.prediction_key->owner_id.value());
            writer.write_u32(entity.prediction_key->input_id.value());
            writer.write_u16(entity.prediction_key->child_ordinal);
        }
    }
    writer.write_u8(static_cast<std::uint8_t>(message.authority_receipts.size()));
    for (const auto& receipt : message.authority_receipts) {
        writer.write_u32(receipt.sequence_id.value());
        encode_authority_event(writer, receipt.event);
    }
}

[[nodiscard]] DecodeResult decode_client_hello(Reader& reader) {
    std::uint8_t requested_role{};
    if (!reader.read_u8(requested_role)) {
        return CodecError::Truncated;
    }
    if (reader.remaining() != 0) {
        return CodecError::TrailingBytes;
    }
    ClientHello message{
        .requested_role = static_cast<JoinRole>(requested_role),
    };
    if (const auto error = validate(message)) {
        return *error;
    }
    return Message{message};
}

[[nodiscard]] DecodeResult decode_server_welcome(Reader& reader) {
    std::uint32_t client_id{};
    std::uint8_t accepted_role{};
    ServerWelcome message;
    if (!reader.read_u32(client_id) || !reader.read_u8(accepted_role) ||
        !reader.read_u32(message.server_tick) || !reader.read_u32(message.respawn_cooldown_ticks) ||
        !decode_rules(reader, message.world_rules)) {
        return CodecError::Truncated;
    }
    if (reader.remaining() != 0) {
        return CodecError::TrailingBytes;
    }
    message.client_id = ClientId{client_id};
    message.accepted_role = static_cast<JoinRole>(accepted_role);
    if (const auto error = validate(message)) {
        return *error;
    }
    return Message{message};
}

[[nodiscard]] DecodeResult decode_client_status(Reader& reader) {
    std::uint32_t last_received_snapshot_id{};
    std::uint32_t last_received_authority_receipt_sequence{};
    if (!reader.read_u32(last_received_snapshot_id) ||
        !reader.read_u32(last_received_authority_receipt_sequence)) {
        return CodecError::Truncated;
    }
    if (reader.remaining() != 0) {
        return CodecError::TrailingBytes;
    }
    ClientStatus message{
        .last_received_snapshot_id = SnapshotId{last_received_snapshot_id},
        .last_received_authority_receipt_sequence =
            AuthorityReceiptSequenceId{last_received_authority_receipt_sequence},
    };
    if (const auto error = validate(message)) {
        return *error;
    }
    return Message{message};
}

[[nodiscard]] DecodeResult decode_input_packet(Reader& reader) {
    std::uint8_t sample_count{};
    std::uint32_t last_received_snapshot_id{};
    std::uint32_t last_received_authority_receipt_sequence{};
    if (!reader.read_u8(sample_count)) {
        return CodecError::Truncated;
    }
    if (sample_count == 0 || sample_count > kMaximumInputSamplesPerPacket) {
        return CodecError::OutOfRange;
    }
    const auto expected_remaining =
        (2U * sizeof(std::uint32_t)) + (static_cast<std::size_t>(sample_count) * kInputSampleBytes);
    if (reader.remaining() < expected_remaining) {
        return CodecError::Truncated;
    }
    if (reader.remaining() > expected_remaining) {
        return CodecError::TrailingBytes;
    }

    InputPacket message;
    if (!reader.read_u32(last_received_snapshot_id) ||
        !reader.read_u32(last_received_authority_receipt_sequence)) {
        return CodecError::Truncated;
    }
    message.last_received_snapshot_id = SnapshotId{last_received_snapshot_id};
    message.last_received_authority_receipt_sequence =
        AuthorityReceiptSequenceId{last_received_authority_receipt_sequence};
    message.samples.reserve(sample_count);
    for (std::uint8_t index = 0; index < sample_count; ++index) {
        std::uint32_t sequence_id{};
        InputSample sample;
        if (!reader.read_u32(sequence_id) || !reader.read_u32(sample.client_tick) ||
            !reader.read_float(sample.movement_x) || !reader.read_float(sample.movement_y) ||
            !reader.read_u16(sample.action_bits)) {
            return CodecError::Truncated;
        }
        sample.sequence_id = InputSequenceId{sequence_id};
        message.samples.push_back(sample);
    }
    if (const auto error = validate(message)) {
        return *error;
    }
    return Message{message};
}

[[nodiscard]] std::optional<CodecError> decode_authority_event(Reader& reader,
                                                               AuthorityEvent& event) {
    std::uint8_t encoded_kind{};
    if (!reader.read_u8(encoded_kind)) {
        return CodecError::Truncated;
    }
    const auto kind = static_cast<AuthorityEventKind>(encoded_kind);
    if (!is_known(kind)) {
        return CodecError::InvalidEnum;
    }
    switch (kind) {
    case AuthorityEventKind::FoodConsumed: {
        FoodConsumed value;
        std::uint32_t food_entity_id{};
        std::uint32_t consumer_entity_id{};
        std::uint32_t consumer_owner_id{};
        if (!reader.read_u32(value.server_tick) || !reader.read_u32(food_entity_id) ||
            !reader.read_u32(consumer_entity_id) || !reader.read_u32(consumer_owner_id) ||
            !reader.read_float(value.food_position_x) ||
            !reader.read_float(value.food_position_y) ||
            !reader.read_float(value.transferred_mass)) {
            return CodecError::Truncated;
        }
        value.food_entity_id = EntityId{food_entity_id};
        value.consumer_entity_id = EntityId{consumer_entity_id};
        value.consumer_owner_id = PlayerOwnerId{consumer_owner_id};
        event = value;
        return std::nullopt;
    }
    case AuthorityEventKind::PlayerAbsorbed: {
        PlayerAbsorbed value;
        if (!decode_absorption(reader, value)) {
            return CodecError::Truncated;
        }
        event = value;
        return std::nullopt;
    }
    case AuthorityEventKind::PlayerSplit: {
        PlayerSplit value;
        std::uint32_t owner_id{};
        std::uint32_t input_id{};
        std::uint32_t parent_entity_id{};
        std::uint32_t child_entity_id{};
        if (!reader.read_u32(value.server_tick) || !reader.read_u32(owner_id) ||
            !reader.read_u32(input_id) || !reader.read_u16(value.child_ordinal) ||
            !reader.read_u32(parent_entity_id) || !reader.read_u32(child_entity_id) ||
            !reader.read_float(value.origin_position_x) ||
            !reader.read_float(value.origin_position_y) ||
            !reader.read_float(value.initial_launch_velocity_x) ||
            !reader.read_float(value.initial_launch_velocity_y) ||
            !reader.read_float(value.parent_mass) || !reader.read_float(value.child_mass)) {
            return CodecError::Truncated;
        }
        value.owner_id = PlayerOwnerId{owner_id};
        value.input_id = InputSequenceId{input_id};
        value.parent_entity_id = EntityId{parent_entity_id};
        value.child_entity_id = EntityId{child_entity_id};
        event = value;
        return std::nullopt;
    }
    case AuthorityEventKind::PiecesMerged: {
        PiecesMerged value;
        std::uint32_t owner_id{};
        std::uint32_t survivor_entity_id{};
        std::uint32_t consumed_entity_id{};
        if (!reader.read_u32(value.server_tick) || !reader.read_u32(owner_id) ||
            !reader.read_u32(survivor_entity_id) || !reader.read_u32(consumed_entity_id) ||
            !reader.read_float(value.combined_mass)) {
            return CodecError::Truncated;
        }
        value.owner_id = PlayerOwnerId{owner_id};
        value.survivor_entity_id = EntityId{survivor_entity_id};
        value.consumed_entity_id = EntityId{consumed_entity_id};
        event = value;
        return std::nullopt;
    }
    }
    return CodecError::InvalidEnum;
}

[[nodiscard]] DecodeResult decode_full_snapshot(Reader& reader) {
    std::uint32_t snapshot_id{};
    std::uint32_t last_processed_input_id{};
    std::uint32_t input_receive_through{};
    std::uint32_t authority_receipts_retired_through{};
    std::uint8_t session_mode{};
    std::uint32_t next_entity_id{};
    std::uint32_t primary_entity_id{};
    std::uint32_t follow_entity_id{};
    std::uint32_t defeat_tick{};
    std::uint32_t respawn_available_tick{};
    std::uint32_t latest_respawn_request_id{};
    std::uint8_t latest_respawn_result{};
    std::uint16_t owned_entity_count{};
    std::uint8_t has_latest_absorption{};
    FullSnapshot message{};
    if (!reader.read_u32(snapshot_id) || !reader.read_u32(message.server_tick) ||
        !reader.read_u32(last_processed_input_id) || !reader.read_u32(input_receive_through) ||
        !reader.read_u8(message.pending_input_count) ||
        !reader.read_u32(authority_receipts_retired_through) ||
        !reader.read_u16(message.checkpoint_schema_id) ||
        !reader.read_u64(message.checkpoint_digest) || !reader.read_u32(next_entity_id) ||
        !reader.read_u8(session_mode) || !reader.read_u32(primary_entity_id) ||
        !reader.read_u32(follow_entity_id) || !reader.read_u32(defeat_tick) ||
        !reader.read_u32(respawn_available_tick) || !reader.read_u32(latest_respawn_request_id) ||
        !reader.read_u8(latest_respawn_result) || !reader.read_u16(owned_entity_count) ||
        !reader.read_u8(has_latest_absorption)) {
        return CodecError::Truncated;
    }
    if (has_latest_absorption > 1U) {
        return CodecError::OutOfRange;
    }
    const auto session_tail_bytes =
        (static_cast<std::size_t>(owned_entity_count) * kOwnedEntityIdBytes) +
        (has_latest_absorption != 0U ? kPlayerAbsorbedBytes : 0U) + sizeof(std::uint16_t);
    if (reader.remaining() < session_tail_bytes) {
        return CodecError::Truncated;
    }

    message.snapshot_id = SnapshotId{snapshot_id};
    message.last_processed_input_id = InputSequenceId{last_processed_input_id};
    message.input_receive_through = InputSequenceId{input_receive_through};
    message.authority_receipts_retired_through =
        AuthorityReceiptSequenceId{authority_receipts_retired_through};
    message.next_entity_id = EntityId{next_entity_id};
    message.recipient.mode = static_cast<SessionMode>(session_mode);
    message.recipient.primary_entity_id = EntityId{primary_entity_id};
    message.recipient.follow_entity_id = EntityId{follow_entity_id};
    if (defeat_tick != std::numeric_limits<std::uint32_t>::max()) {
        message.recipient.defeat_tick = defeat_tick;
    }
    if (respawn_available_tick != std::numeric_limits<std::uint32_t>::max()) {
        message.recipient.respawn_available_tick = respawn_available_tick;
    }
    message.recipient.latest_respawn_request_id = InputSequenceId{latest_respawn_request_id};
    message.recipient.latest_respawn_result = static_cast<RespawnResult>(latest_respawn_result);
    message.recipient.owned_entity_ids.reserve(owned_entity_count);
    for (std::uint16_t index = 0; index < owned_entity_count; ++index) {
        std::uint32_t entity_id{};
        if (!reader.read_u32(entity_id)) {
            return CodecError::Truncated;
        }
        message.recipient.owned_entity_ids.emplace_back(entity_id);
    }
    if (has_latest_absorption != 0U) {
        PlayerAbsorbed event;
        if (!decode_absorption(reader, event)) {
            return CodecError::Truncated;
        }
        message.recipient.latest_absorption = event;
    }

    std::uint16_t owner_count{};
    if (!reader.read_u16(owner_count)) {
        return CodecError::Truncated;
    }
    if (reader.remaining() <
        (static_cast<std::size_t>(owner_count) * kOwnerStateBytes) + sizeof(std::uint16_t)) {
        return CodecError::Truncated;
    }
    message.owners.reserve(owner_count);
    for (std::uint16_t index = 0; index < owner_count; ++index) {
        std::uint32_t owner_id{};
        std::uint32_t last_input_id{};
        OwnerState owner;
        if (!reader.read_u32(owner_id) || !reader.read_float(owner.movement_x) ||
            !reader.read_float(owner.movement_y) ||
            !reader.read_float(owner.last_non_zero_movement_x) ||
            !reader.read_float(owner.last_non_zero_movement_y) || !reader.read_u32(last_input_id) ||
            !reader.read_u32(owner.split_cooldown_end_tick)) {
            return CodecError::Truncated;
        }
        owner.owner_id = PlayerOwnerId{owner_id};
        owner.last_input_id = InputSequenceId{last_input_id};
        message.owners.push_back(owner);
    }

    std::uint16_t entity_count{};
    if (!reader.read_u16(entity_count)) {
        return CodecError::Truncated;
    }
    if (reader.remaining() <
        (static_cast<std::size_t>(entity_count) * kEntityStateBaseBytes) + sizeof(std::uint8_t)) {
        return CodecError::Truncated;
    }
    message.entities.reserve(entity_count);
    for (std::uint16_t index = 0; index < entity_count; ++index) {
        std::uint32_t entity_id{};
        std::uint8_t entity_kind{};
        std::uint32_t owner_id{};
        std::uint8_t has_prediction_key{};
        EntityState entity;
        if (!reader.read_u32(entity_id) || !reader.read_u8(entity_kind) ||
            !reader.read_u32(owner_id) || !reader.read_float(entity.position_x) ||
            !reader.read_float(entity.position_y) || !reader.read_float(entity.mass) ||
            !reader.read_float(entity.launch_velocity_x) ||
            !reader.read_float(entity.launch_velocity_y) ||
            !reader.read_u32(entity.merge_eligible_tick) || !reader.read_u8(has_prediction_key)) {
            return CodecError::Truncated;
        }
        if (has_prediction_key > 1U) {
            return CodecError::OutOfRange;
        }
        entity.entity_id = EntityId{entity_id};
        entity.kind = static_cast<EntityKind>(entity_kind);
        entity.owner_id = PlayerOwnerId{owner_id};
        if (has_prediction_key != 0U) {
            std::uint32_t prediction_owner_id{};
            std::uint32_t prediction_input_id{};
            PredictionKey prediction_key;
            if (!reader.read_u32(prediction_owner_id) || !reader.read_u32(prediction_input_id) ||
                !reader.read_u16(prediction_key.child_ordinal)) {
                return CodecError::Truncated;
            }
            prediction_key.owner_id = PlayerOwnerId{prediction_owner_id};
            prediction_key.input_id = InputSequenceId{prediction_input_id};
            entity.prediction_key = prediction_key;
        }
        message.entities.push_back(entity);
    }

    std::uint8_t receipt_count{};
    if (!reader.read_u8(receipt_count)) {
        return CodecError::Truncated;
    }
    if (receipt_count > kMaximumAuthorityReceiptsPerSnapshot) {
        return CodecError::TooManyReceipts;
    }
    message.authority_receipts.reserve(receipt_count);
    for (std::uint8_t index = 0; index < receipt_count; ++index) {
        std::uint32_t receipt_sequence{};
        AuthorityReceipt receipt;
        if (!reader.read_u32(receipt_sequence)) {
            return CodecError::Truncated;
        }
        if (const auto error = decode_authority_event(reader, receipt.event)) {
            return *error;
        }
        receipt.sequence_id = AuthorityReceiptSequenceId{receipt_sequence};
        message.authority_receipts.push_back(receipt);
    }
    if (reader.remaining() != 0) {
        return CodecError::TrailingBytes;
    }
    if (const auto error = validate(message)) {
        return *error;
    }
    return Message{std::move(message)};
}

} // namespace

EncodeResult encode(const Message& message) {
    if (const auto error = validate_message(message)) {
        return *error;
    }

    const auto encoded_payload_size = payload_size(message);
    if (encoded_payload_size > kMaximumEncodedMessageBytes - kPacketHeaderBytes) {
        return CodecError::MessageTooLarge;
    }

    Writer payload_writer{encoded_payload_size};
    std::visit(
        [&payload_writer](const auto& value) {
            encode_payload(payload_writer, value);
        },
        message);
    const auto payload = payload_writer.take_bytes();

    Writer message_writer{kPacketHeaderBytes + payload.size()};
    message_writer.write_u8(kMagicD);
    message_writer.write_u8(kMagicO);
    message_writer.write_u8(kMagicT);
    message_writer.write_u8(kMagicS);
    message_writer.write_u16(kProtocolVersion);
    message_writer.write_u8(static_cast<std::uint8_t>(message_kind(message)));
    message_writer.write_u8(kSupportedFlags);
    message_writer.write_u32(static_cast<std::uint32_t>(payload.size()));
    message_writer.append(payload);
    return message_writer.take_bytes();
}

std::optional<CodecError> validate(const Message& message) {
    return validate_message(message);
}

DecodeResult decode(std::span<const std::byte> bytes) {
    if (bytes.size() > kMaximumEncodedMessageBytes) {
        return CodecError::MessageTooLarge;
    }
    if (bytes.size() < kPacketHeaderBytes) {
        return CodecError::Truncated;
    }

    Reader header{bytes.first(kPacketHeaderBytes)};
    std::uint8_t magic_d{};
    std::uint8_t magic_o{};
    std::uint8_t magic_t{};
    std::uint8_t magic_s{};
    std::uint16_t version{};
    std::uint8_t encoded_kind{};
    std::uint8_t flags{};
    std::uint32_t encoded_payload_size{};
    if (!header.read_u8(magic_d) || !header.read_u8(magic_o) || !header.read_u8(magic_t) ||
        !header.read_u8(magic_s) || !header.read_u16(version) || !header.read_u8(encoded_kind) ||
        !header.read_u8(flags) || !header.read_u32(encoded_payload_size)) {
        return CodecError::Truncated;
    }
    if (magic_d != kMagicD || magic_o != kMagicO || magic_t != kMagicT || magic_s != kMagicS) {
        return CodecError::InvalidMagic;
    }
    if (version != kProtocolVersion) {
        return CodecError::UnsupportedVersion;
    }
    if (flags != kSupportedFlags) {
        return CodecError::UnsupportedFlags;
    }
    if (encoded_payload_size != bytes.size() - kPacketHeaderBytes) {
        return CodecError::PayloadLengthMismatch;
    }

    Reader payload{bytes.subspan(kPacketHeaderBytes)};
    switch (static_cast<MessageKind>(encoded_kind)) {
    case MessageKind::ClientHello:
        return decode_client_hello(payload);
    case MessageKind::ServerWelcome:
        return decode_server_welcome(payload);
    case MessageKind::InputPacket:
        return decode_input_packet(payload);
    case MessageKind::FullSnapshot:
        return decode_full_snapshot(payload);
    case MessageKind::ClientStatus:
        return decode_client_status(payload);
    }
    return CodecError::UnknownMessageKind;
}

} // namespace dots::protocol

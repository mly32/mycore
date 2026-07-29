#include "dots/protocol/codec.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace {

using dots::protocol::ClientHello;
using dots::protocol::ClientId;
using dots::protocol::CodecError;
using dots::protocol::DecodeResult;
using dots::protocol::EncodedMessage;
using dots::protocol::EntityId;
using dots::protocol::EntityKind;
using dots::protocol::EntityState;
using dots::protocol::FullSnapshot;
using dots::protocol::InputPacket;
using dots::protocol::InputSample;
using dots::protocol::InputSequenceId;
using dots::protocol::Message;
using dots::protocol::OwnerState;
using dots::protocol::PlayerOwnerId;
using dots::protocol::RecipientSessionState;
using dots::protocol::RespawnResult;
using dots::protocol::ServerWelcome;
using dots::protocol::SessionMode;
using dots::protocol::SnapshotId;
using dots::protocol::WorldRules;

[[nodiscard]] WorldRules world_rules() {
    return {
        .initial_player_mass = 16.0F,
        .food_mass = 1.0F,
        .spatial_grid_cell_size = 8.0F,
        .player_speed_units_per_second = 6.0F,
        .split_recast_ticks = 15,
        .merge_delay_ticks = 150,
        .maximum_pieces_per_owner = 8,
        .minimum_split_mass = 16.0F,
        .child_launch_speed_units_per_second = 18.0F,
        .launch_decay_units_per_second_squared = 18.0F,
        .cohesion_speed_units_per_second = 3.0F,
    };
}

[[nodiscard]] OwnerState owner_state(PlayerOwnerId owner_id = PlayerOwnerId{7}) {
    return {
        .owner_id = owner_id,
    };
}

[[nodiscard]] EncodedMessage byte_sequence(std::initializer_list<std::uint8_t> values) {
    EncodedMessage bytes;
    bytes.reserve(values.size());
    for (const auto value : values) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

[[nodiscard]] EncodedMessage require_encode(const Message& message) {
    auto result = dots::protocol::encode(message);
    auto* bytes = std::get_if<EncodedMessage>(&result);
    if (bytes == nullptr) {
        FAIL("Expected protocol encoding to succeed");
        return {};
    }
    return std::move(*bytes);
}

[[nodiscard]] Message require_decode(std::span<const std::byte> bytes) {
    auto result = dots::protocol::decode(bytes);
    auto* message = std::get_if<Message>(&result);
    if (message == nullptr) {
        FAIL("Expected protocol decoding to succeed");
        return ClientHello{};
    }
    return std::move(*message);
}

void require_encode_error(const Message& message, CodecError expected) {
    const auto result = dots::protocol::encode(message);
    const auto* error = std::get_if<CodecError>(&result);
    REQUIRE(error != nullptr);
    CHECK(*error == expected);
}

void require_decode_error(std::span<const std::byte> bytes, CodecError expected) {
    const DecodeResult result = dots::protocol::decode(bytes);
    const auto* error = std::get_if<CodecError>(&result);
    REQUIRE(error != nullptr);
    CHECK(*error == expected);
}

void write_u16(EncodedMessage& bytes, std::size_t offset, std::uint16_t value) {
    REQUIRE(offset + 2 <= bytes.size());
    bytes[offset] = static_cast<std::byte>(value >> 8U);
    bytes[offset + 1] = static_cast<std::byte>(value);
}

void write_u32(EncodedMessage& bytes, std::size_t offset, std::uint32_t value) {
    REQUIRE(offset + 4 <= bytes.size());
    bytes[offset] = static_cast<std::byte>(value >> 24U);
    bytes[offset + 1] = static_cast<std::byte>(value >> 16U);
    bytes[offset + 2] = static_cast<std::byte>(value >> 8U);
    bytes[offset + 3] = static_cast<std::byte>(value);
}

[[nodiscard]] InputSample input_sample(std::uint32_t sequence_id = 0x01020304,
                                       std::uint32_t client_tick = 0x05060708) {
    return {
        .sequence_id = InputSequenceId{sequence_id},
        .client_tick = client_tick,
        .movement_x = 0.5F,
        .movement_y = -0.25F,
        .action_bits = 0,
    };
}

[[nodiscard]] InputPacket input_fixture() {
    return {
        .last_received_snapshot_id = SnapshotId{0x0A0B0C0D},
        .last_received_authority_receipt_sequence =
            dots::protocol::AuthorityReceiptSequenceId::invalid(),
        .samples = {input_sample()},
    };
}

[[nodiscard]] FullSnapshot snapshot_fixture() {
    return {
        .snapshot_id = SnapshotId{1},
        .server_tick = 2,
        .last_processed_input_id = InputSequenceId::invalid(),
        .pending_input_count = 3,
        .recipient =
            {
                .mode = SessionMode::Playing,
                .owned_entity_ids = {EntityId{1}},
                .primary_entity_id = EntityId{1},
            },
        .owners = {owner_state()},
        .entities =
            {
                {
                    .entity_id = EntityId{1},
                    .kind = EntityKind::Player,
                    .owner_id = PlayerOwnerId{7},
                    .position_x = 1.0F,
                    .position_y = -2.0F,
                    .mass = 16.0F,
                },
                {
                    .entity_id = EntityId{2},
                    .kind = EntityKind::Food,
                    .position_x = 0.5F,
                    .position_y = 0.0F,
                    .mass = 1.0F,
                },
            },
    };
}

} // namespace

TEST_CASE("Protocol messages round trip", "[dots][protocol]") {
    const std::vector<Message> messages{
        ClientHello{},
        ServerWelcome{
            .client_id = ClientId{7},
            .server_tick = 42,
            .respawn_cooldown_ticks = 90,
            .world_rules = world_rules(),
        },
        input_fixture(),
        InputPacket{
            .last_received_snapshot_id = SnapshotId{1},
            .last_received_authority_receipt_sequence =
                dots::protocol::AuthorityReceiptSequenceId::invalid(),
            .samples = {input_sample(1, 1), input_sample(2, 2), input_sample(3, 3)},
        },
        snapshot_fixture(),
    };

    for (const auto& message : messages) {
        CHECK(require_decode(require_encode(message)) == message);
    }
}

TEST_CASE("Input packet encoding has stable golden bytes", "[dots][protocol][golden]") {
    const auto expected = byte_sequence({
        0x44, 0x4F, 0x54, 0x53, 0x00, 0x04, 0x03, 0x00, 0x00, 0x00, 0x00, 0x1B, 0x01,
        0x0A, 0x0B, 0x0C, 0x0D, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x02, 0x03, 0x04, 0x05,
        0x06, 0x07, 0x08, 0x3F, 0x00, 0x00, 0x00, 0xBE, 0x80, 0x00, 0x00, 0x00, 0x00,
    });

    CHECK(require_encode(input_fixture()) == expected);
    CHECK(require_decode(expected) == Message{input_fixture()});

    const InputPacket maximum{
        .last_received_snapshot_id = SnapshotId{1},
        .last_received_authority_receipt_sequence =
            dots::protocol::AuthorityReceiptSequenceId::invalid(),
        .samples = {input_sample(1, 1), input_sample(2, 2), input_sample(3, 3)},
    };
    CHECK(require_encode(maximum).size() == dots::protocol::kMaximumEncodedInputPacketBytes);
}

TEST_CASE("Full snapshot encoding has stable golden bytes", "[dots][protocol][golden]") {
    const auto expected = byte_sequence({
        0x44, 0x4F, 0x54, 0x53, 0x00, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0xA1, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00,
        0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x07, 0x3F, 0x80, 0x00, 0x00, 0xC0, 0x00, 0x00,
        0x00, 0x41, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0x3F, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    });

    CHECK(require_encode(snapshot_fixture()) == expected);
    CHECK(require_decode(expected) == Message{snapshot_fixture()});
}

TEST_CASE("Protocol decoder rejects malformed framing", "[dots][protocol][validation]") {
    const auto hello = require_encode(ClientHello{});
    for (std::size_t size = 0; size < dots::protocol::kPacketHeaderBytes; ++size) {
        require_decode_error(std::span{hello}.first(size), CodecError::Truncated);
    }

    auto bad_magic = hello;
    bad_magic[0] = std::byte{0};
    require_decode_error(bad_magic, CodecError::InvalidMagic);

    auto bad_version = hello;
    write_u16(bad_version, 4, dots::protocol::kProtocolVersion + 1);
    require_decode_error(bad_version, CodecError::UnsupportedVersion);

    auto version_one = hello;
    write_u16(version_one, 4, 1);
    require_decode_error(version_one, CodecError::UnsupportedVersion);

    auto bad_kind = hello;
    bad_kind[6] = std::byte{0x7F};
    require_decode_error(bad_kind, CodecError::UnknownMessageKind);

    auto bad_flags = hello;
    bad_flags[7] = std::byte{1};
    require_decode_error(bad_flags, CodecError::UnsupportedFlags);

    auto false_length = hello;
    write_u32(false_length, 8, 1);
    require_decode_error(false_length, CodecError::PayloadLengthMismatch);

    auto trailing = hello;
    trailing.push_back(std::byte{0});
    write_u32(trailing, 8, 1);
    require_decode_error(trailing, CodecError::TrailingBytes);

    auto truncated_payload = require_encode(input_fixture());
    truncated_payload.resize(dots::protocol::kPacketHeaderBytes + 1);
    write_u32(truncated_payload, 8, 1);
    require_decode_error(truncated_payload, CodecError::Truncated);
}

TEST_CASE("Protocol encoder rejects invalid message values", "[dots][protocol][validation]") {
    require_encode_error(ServerWelcome{}, CodecError::InvalidId);

    auto input = input_fixture();
    input.last_received_snapshot_id = SnapshotId::invalid();
    require_encode_error(input, CodecError::InvalidId);
    input = input_fixture();
    input.samples.clear();
    require_encode_error(input, CodecError::OutOfRange);
    input = input_fixture();
    input.samples.resize(dots::protocol::kMaximumInputSamplesPerPacket + 1, input_sample());
    require_encode_error(input, CodecError::OutOfRange);
    input = input_fixture();
    input.samples.front().sequence_id = InputSequenceId::invalid();
    require_encode_error(input, CodecError::InvalidId);
    input = input_fixture();
    input.samples.front().action_bits = 4;
    require_encode_error(input, CodecError::OutOfRange);
    input = input_fixture();
    input.samples.front().movement_x = std::numeric_limits<float>::infinity();
    require_encode_error(input, CodecError::InvalidNumber);
    input = input_fixture();
    input.samples.front().movement_x = 1.0F;
    input.samples.front().movement_y = 1.0F;
    require_encode_error(input, CodecError::OutOfRange);
    input = input_fixture();
    input.samples.push_back(input_sample(0x01020306, 0x05060709));
    require_encode_error(input, CodecError::InvalidInputOrdering);
    input = input_fixture();
    input.samples.push_back(input_sample(0x01020305, 0x0506070A));
    require_encode_error(input, CodecError::InvalidInputOrdering);

    auto snapshot = snapshot_fixture();
    snapshot.snapshot_id = SnapshotId::invalid();
    require_encode_error(snapshot, CodecError::InvalidId);
    snapshot = snapshot_fixture();
    snapshot.pending_input_count = dots::protocol::kMaximumPendingInputCount + 1;
    require_encode_error(snapshot, CodecError::OutOfRange);
    snapshot = snapshot_fixture();
    snapshot.recipient.mode = static_cast<SessionMode>(99);
    require_encode_error(snapshot, CodecError::InvalidEnum);
    snapshot = snapshot_fixture();
    snapshot.recipient.primary_entity_id = EntityId::invalid();
    require_encode_error(snapshot, CodecError::InvalidId);
    snapshot = snapshot_fixture();
    snapshot.recipient.owned_entity_ids.push_back(EntityId{1});
    require_encode_error(snapshot, CodecError::DuplicateEntity);
    snapshot = snapshot_fixture();
    snapshot.recipient.latest_respawn_request_id = InputSequenceId{7};
    require_encode_error(snapshot, CodecError::InvalidId);
    snapshot = snapshot_fixture();
    snapshot.recipient.latest_respawn_request_id = InputSequenceId{7};
    snapshot.recipient.latest_respawn_result = RespawnResult::Accepted;
    snapshot.last_processed_input_id = InputSequenceId{7};
    CHECK(require_decode(require_encode(snapshot)) == Message{snapshot});
    snapshot = snapshot_fixture();
    snapshot.entities[0].kind = static_cast<EntityKind>(99);
    require_encode_error(snapshot, CodecError::InvalidEnum);
    snapshot = snapshot_fixture();
    snapshot.entities[0].owner_id = PlayerOwnerId::invalid();
    require_encode_error(snapshot, CodecError::InvalidId);
    snapshot = snapshot_fixture();
    snapshot.entities[1].owner_id = PlayerOwnerId{4};
    require_encode_error(snapshot, CodecError::InvalidCheckpoint);
    snapshot = snapshot_fixture();
    snapshot.entities[0].position_x = std::numeric_limits<float>::quiet_NaN();
    require_encode_error(snapshot, CodecError::InvalidNumber);
    snapshot = snapshot_fixture();
    snapshot.entities[0].mass = 0.0F;
    require_encode_error(snapshot, CodecError::OutOfRange);
    snapshot = snapshot_fixture();
    snapshot.entities[1].entity_id = snapshot.entities[0].entity_id;
    require_encode_error(snapshot, CodecError::DuplicateEntity);
}

TEST_CASE("Protocol validates version 4 lifecycle state and input actions",
          "[dots][protocol][lifecycle][validation]") {
    auto respawn = input_fixture();
    respawn.samples.front().action_bits = dots::protocol::kRespawnActionBit;
    CHECK(require_decode(require_encode(respawn)) == Message{respawn});
    respawn.samples.front().action_bits = dots::protocol::kKnownInputActionBits << 1U;
    require_encode_error(respawn, CodecError::OutOfRange);

    FullSnapshot spectating{
        .snapshot_id = SnapshotId{4},
        .server_tick = 12,
        .last_processed_input_id = InputSequenceId{8},
        .recipient =
            {
                .mode = SessionMode::Spectating,
                .follow_entity_id = EntityId{1},
                .defeat_tick = 10,
                .respawn_available_tick = 100,
                .latest_absorption =
                    dots::protocol::PlayerAbsorbed{
                        .server_tick = 10,
                        .absorber_entity_id = EntityId{1},
                        .victim_entity_id = EntityId{2},
                        .absorber_owner_id = PlayerOwnerId{7},
                        .victim_owner_id = PlayerOwnerId{8},
                        .transferred_mass = 16.0F,
                    },
                .latest_respawn_request_id = InputSequenceId{8},
                .latest_respawn_result = RespawnResult::RejectedCooldown,
            },
        .owners = {owner_state()},
        .entities = {{
            .entity_id = EntityId{1},
            .kind = EntityKind::Player,
            .owner_id = PlayerOwnerId{7},
            .mass = 32.0F,
        }},
    };
    CHECK(require_decode(require_encode(spectating)) == Message{spectating});

    auto invalid = spectating;
    invalid.recipient.respawn_available_tick = 9;
    require_encode_error(invalid, CodecError::OutOfRange);
    invalid = spectating;
    invalid.recipient.follow_entity_id = EntityId{99};
    require_encode_error(invalid, CodecError::InvalidId);
    invalid = spectating;
    invalid.recipient.latest_respawn_result = static_cast<RespawnResult>(99);
    require_encode_error(invalid, CodecError::InvalidEnum);
    invalid = spectating;
    invalid.recipient.latest_absorption->transferred_mass = std::numeric_limits<float>::quiet_NaN();
    require_encode_error(invalid, CodecError::InvalidNumber);
    invalid = spectating;
    invalid.recipient.latest_absorption->transferred_mass = 0.0F;
    require_encode_error(invalid, CodecError::OutOfRange);
    invalid = spectating;
    invalid.recipient.latest_absorption->server_tick = 13;
    require_encode_error(invalid, CodecError::OutOfRange);
    invalid = spectating;
    invalid.recipient.latest_respawn_request_id = InputSequenceId{9};
    require_encode_error(invalid, CodecError::InvalidInputOrdering);
}

TEST_CASE("Protocol version 4 carries complete checkpoint and authority receipt state",
          "[dots][protocol][rollback]") {
    auto snapshot = snapshot_fixture();
    snapshot.server_tick = 20;
    snapshot.checkpoint_digest = 0x0102030405060708ULL;
    snapshot.owners[0] = {
        .owner_id = PlayerOwnerId{7},
        .movement_x = 0.5F,
        .last_non_zero_movement_x = 1.0F,
        .last_input_id = InputSequenceId{10},
        .split_cooldown_end_tick = 15,
    };
    snapshot.entities[0].launch_velocity_x = 3.0F;
    snapshot.entities[0].merge_eligible_tick = 20;
    snapshot.entities[0].prediction_key = dots::protocol::PredictionKey{
        .owner_id = PlayerOwnerId{7},
        .input_id = InputSequenceId{9},
        .child_ordinal = 1,
    };
    snapshot.authority_receipts_retired_through = dots::protocol::AuthorityReceiptSequenceId{3};
    snapshot.authority_receipts = {
        {
            .sequence_id = dots::protocol::AuthorityReceiptSequenceId{4},
            .event =
                dots::protocol::FoodConsumed{
                    .server_tick = 17,
                    .food_entity_id = EntityId{90},
                    .consumer_entity_id = EntityId{1},
                    .consumer_owner_id = PlayerOwnerId{7},
                    .transferred_mass = 1.0F,
                },
        },
        {
            .sequence_id = dots::protocol::AuthorityReceiptSequenceId{5},
            .event =
                dots::protocol::PlayerAbsorbed{
                    .server_tick = 18,
                    .absorber_entity_id = EntityId{1},
                    .victim_entity_id = EntityId{91},
                    .absorber_owner_id = PlayerOwnerId{7},
                    .victim_owner_id = PlayerOwnerId{8},
                    .transferred_mass = 16.0F,
                },
        },
        {
            .sequence_id = dots::protocol::AuthorityReceiptSequenceId{6},
            .event =
                dots::protocol::PlayerSplit{
                    .server_tick = 19,
                    .owner_id = PlayerOwnerId{7},
                    .input_id = InputSequenceId{10},
                    .child_ordinal = 0,
                    .parent_entity_id = EntityId{1},
                    .child_entity_id = EntityId{92},
                    .parent_mass = 8.0F,
                    .child_mass = 8.0F,
                },
        },
        {
            .sequence_id = dots::protocol::AuthorityReceiptSequenceId{7},
            .event =
                dots::protocol::PiecesMerged{
                    .server_tick = 20,
                    .owner_id = PlayerOwnerId{7},
                    .survivor_entity_id = EntityId{1},
                    .consumed_entity_id = EntityId{92},
                    .combined_mass = 16.0F,
                },
        },
    };

    CHECK(require_decode(require_encode(snapshot)) == Message{snapshot});

    auto invalid = snapshot;
    invalid.checkpoint_schema_id = dots::protocol::kCheckpointSchemaId + 1;
    require_encode_error(invalid, CodecError::InvalidCheckpoint);
    invalid = snapshot;
    invalid.owners.clear();
    require_encode_error(invalid, CodecError::InvalidId);
    invalid = snapshot;
    invalid.owners.push_back(invalid.owners.front());
    require_encode_error(invalid, CodecError::DuplicateOwner);
    invalid = snapshot;
    invalid.entities[0].prediction_key->owner_id = PlayerOwnerId{8};
    require_encode_error(invalid, CodecError::InvalidCheckpoint);
    invalid = snapshot;
    invalid.entities[1].prediction_key = dots::protocol::PredictionKey{
        .owner_id = PlayerOwnerId{7},
        .input_id = InputSequenceId{9},
    };
    require_encode_error(invalid, CodecError::InvalidCheckpoint);
    invalid = snapshot;
    invalid.authority_receipts[1].sequence_id = dots::protocol::AuthorityReceiptSequenceId{9};
    require_encode_error(invalid, CodecError::InvalidReceiptOrdering);
    invalid = snapshot;
    invalid.authority_receipts_retired_through = dots::protocol::AuthorityReceiptSequenceId{2};
    require_encode_error(invalid, CodecError::InvalidReceiptOrdering);
    invalid = snapshot;
    std::get<dots::protocol::FoodConsumed>(invalid.authority_receipts[0].event).server_tick = 21;
    require_encode_error(invalid, CodecError::OutOfRange);
    invalid = snapshot;
    invalid.authority_receipts[1].event = invalid.authority_receipts[0].event;
    require_encode_error(invalid, CodecError::InvalidCheckpoint);
    invalid = snapshot;
    invalid.authority_receipts.resize(dots::protocol::kMaximumAuthorityReceiptsPerSnapshot + 1,
                                      invalid.authority_receipts.front());
    require_encode_error(invalid, CodecError::TooManyReceipts);

    auto hostile = require_encode(snapshot);
    write_u16(hostile, 29, dots::protocol::kCheckpointSchemaId + 1);
    require_decode_error(hostile, CodecError::InvalidCheckpoint);

    hostile = require_encode(snapshot);
    hostile[187] = std::byte{0x7F};
    require_decode_error(hostile, CodecError::InvalidEnum);
}

TEST_CASE("Protocol decoder rejects invalid payload values", "[dots][protocol][validation]") {
    auto welcome = require_encode(ServerWelcome{
        .client_id = ClientId{1},
        .world_rules = world_rules(),
    });
    write_u32(welcome, 12, ClientId::kInvalidValue);
    require_decode_error(welcome, CodecError::InvalidId);

    welcome = require_encode(ServerWelcome{
        .client_id = ClientId{1},
        .world_rules = world_rules(),
    });
    write_u32(welcome, 24, 0);
    require_decode_error(welcome, CodecError::InvalidCheckpoint);

    auto input = require_encode(input_fixture());
    write_u32(input, 13, SnapshotId::kInvalidValue);
    require_decode_error(input, CodecError::InvalidId);

    input = require_encode(input_fixture());
    write_u32(input, 21, InputSequenceId::kInvalidValue);
    require_decode_error(input, CodecError::InvalidId);

    input = require_encode(input_fixture());
    write_u16(input, 37, 4);
    require_decode_error(input, CodecError::OutOfRange);

    input = require_encode(input_fixture());
    write_u32(input, 29, 0x7F800000);
    require_decode_error(input, CodecError::InvalidNumber);

    input = require_encode(input_fixture());
    write_u32(input, 33, 0x3F800000);
    require_decode_error(input, CodecError::OutOfRange);

    input = require_encode(input_fixture());
    input[12] = std::byte{0};
    require_decode_error(input, CodecError::OutOfRange);

    input = require_encode(input_fixture());
    input[12] = std::byte{4};
    require_decode_error(input, CodecError::OutOfRange);

    const InputPacket ordered{
        .last_received_snapshot_id = SnapshotId{1},
        .last_received_authority_receipt_sequence =
            dots::protocol::AuthorityReceiptSequenceId::invalid(),
        .samples = {input_sample(1, 1), input_sample(2, 2)},
    };
    input = require_encode(ordered);
    write_u32(input, 39, 3);
    require_decode_error(input, CodecError::InvalidInputOrdering);

    input = require_encode(ordered);
    write_u32(input, 43, 3);
    require_decode_error(input, CodecError::InvalidInputOrdering);

    input = require_encode(ordered);
    input[12] = std::byte{1};
    require_decode_error(input, CodecError::TrailingBytes);

    input = require_encode(input_fixture());
    input[12] = std::byte{2};
    require_decode_error(input, CodecError::Truncated);

    auto snapshot = require_encode(snapshot_fixture());
    write_u32(snapshot, 12, SnapshotId::kInvalidValue);
    require_decode_error(snapshot, CodecError::InvalidId);

    snapshot = require_encode(snapshot_fixture());
    snapshot[24] = static_cast<std::byte>(dots::protocol::kMaximumPendingInputCount + 1);
    require_decode_error(snapshot, CodecError::OutOfRange);

    snapshot = require_encode(snapshot_fixture());
    snapshot[43] = std::byte{0x7F};
    require_decode_error(snapshot, CodecError::InvalidEnum);

    snapshot = require_encode(snapshot_fixture());
    write_u32(snapshot, 44, EntityId::kInvalidValue);
    require_decode_error(snapshot, CodecError::InvalidId);

    snapshot = require_encode(snapshot_fixture());
    write_u32(snapshot, 68, EntityId::kInvalidValue);
    require_decode_error(snapshot, CodecError::InvalidId);

    snapshot = require_encode(snapshot_fixture());
    snapshot[108] = std::byte{0x7F};
    require_decode_error(snapshot, CodecError::InvalidEnum);

    snapshot = require_encode(snapshot_fixture());
    write_u32(snapshot, 113, 0x7F800000);
    require_decode_error(snapshot, CodecError::InvalidNumber);

    snapshot = require_encode(snapshot_fixture());
    write_u32(snapshot, 121, 0);
    require_decode_error(snapshot, CodecError::OutOfRange);

    snapshot = require_encode(snapshot_fixture());
    write_u32(snapshot, 138, 1);
    require_decode_error(snapshot, CodecError::DuplicateEntity);

    snapshot = require_encode(snapshot_fixture());
    write_u16(snapshot, 102, 3);
    require_decode_error(snapshot, CodecError::Truncated);

    snapshot = require_encode(snapshot_fixture());
    write_u16(snapshot, 102, 1);
    require_decode_error(snapshot, CodecError::TrailingBytes);
}

TEST_CASE("Protocol distinguishes target budget from hostile input limit",
          "[dots][protocol][budget]") {
    FullSnapshot above_target{
        .snapshot_id = SnapshotId{1},
        .recipient =
            {
                .mode = SessionMode::Spectating,
                .defeat_tick = 0,
                .respawn_available_tick = 0,
            },
    };
    for (std::uint32_t id = 0; id < 70; ++id) {
        above_target.entities.push_back({
            .entity_id = EntityId{id},
            .kind = EntityKind::Food,
            .mass = 1.0F,
        });
    }

    const auto encoded = require_encode(above_target);
    CHECK_FALSE(dots::protocol::fits_target_transport_payload(encoded.size()));
    CHECK(encoded.size() < dots::protocol::kMaximumEncodedMessageBytes);
    CHECK(require_decode(encoded) == Message{above_target});

    FullSnapshot too_large{
        .snapshot_id = SnapshotId{1},
        .recipient =
            {
                .mode = SessionMode::Spectating,
                .defeat_tick = 0,
                .respawn_available_tick = 0,
            },
    };
    for (std::uint32_t id = 0; id < 3'854; ++id) {
        too_large.entities.push_back({
            .entity_id = EntityId{id},
            .kind = EntityKind::Food,
            .mass = 1.0F,
        });
    }
    require_encode_error(too_large, CodecError::MessageTooLarge);

    FullSnapshot too_many_entities{
        .snapshot_id = SnapshotId{1},
    };
    too_many_entities.entities.resize(
        static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1);
    require_encode_error(too_many_entities, CodecError::TooManyEntities);

    const EncodedMessage hostile(dots::protocol::kMaximumEncodedMessageBytes + 1);
    require_decode_error(hostile, CodecError::MessageTooLarge);
}

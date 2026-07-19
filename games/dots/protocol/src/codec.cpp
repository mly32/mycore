#include "dots/protocol/codec.hpp"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
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
constexpr std::size_t kServerWelcomePayloadBytes = 12;
constexpr std::size_t kInputPacketPrefixBytes = 5;
constexpr std::size_t kInputSampleBytes = 18;
constexpr std::size_t kFullSnapshotHeaderBytes = 15;
constexpr std::size_t kEntityStateBytes = 17;
constexpr float kMaximumMovementLengthSquared = 1.0001F;

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

[[nodiscard]] bool valid_movement(float x, float y) noexcept {
    return std::isfinite(x) && std::isfinite(y) && x >= -1.0F && x <= 1.0F && y >= -1.0F &&
           y <= 1.0F && ((x * x) + (y * y)) <= kMaximumMovementLengthSquared;
}

[[nodiscard]] std::optional<CodecError> validate(const ClientHello&) noexcept {
    return std::nullopt;
}

[[nodiscard]] std::optional<CodecError> validate(const ServerWelcome& message) noexcept {
    if (!message.client_id.is_valid() || !message.controlled_entity_id.is_valid()) {
        return CodecError::InvalidId;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<CodecError> validate(const InputSample& sample) noexcept {
    if (!sample.sequence_id.is_valid()) {
        return CodecError::InvalidId;
    }
    if (sample.action_bits != 0) {
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

[[nodiscard]] std::optional<CodecError> validate(const FullSnapshot& message) {
    if (!message.snapshot_id.is_valid()) {
        return CodecError::InvalidId;
    }
    if (message.pending_input_count > kMaximumPendingInputCount) {
        return CodecError::OutOfRange;
    }
    if (message.entities.size() > std::numeric_limits<std::uint16_t>::max()) {
        return CodecError::TooManyEntities;
    }

    std::unordered_set<std::uint32_t> entity_ids;
    entity_ids.reserve(message.entities.size());
    for (const auto& entity : message.entities) {
        if (!entity.entity_id.is_valid()) {
            return CodecError::InvalidId;
        }
        if (!is_known(entity.kind)) {
            return CodecError::InvalidEnum;
        }
        if (!std::isfinite(entity.position_x) || !std::isfinite(entity.position_y) ||
            !std::isfinite(entity.mass)) {
            return CodecError::InvalidNumber;
        }
        if (entity.mass <= 0.0F) {
            return CodecError::OutOfRange;
        }
        if (!entity_ids.insert(entity.entity_id.value()).second) {
            return CodecError::DuplicateEntity;
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
            } else {
                return MessageKind::FullSnapshot;
            }
        },
        message);
}

[[nodiscard]] std::optional<CodecError> validate(const Message& message) {
    return std::visit(
        [](const auto& value) {
            return validate(value);
        },
        message);
}

[[nodiscard]] std::size_t payload_size(const Message& message) {
    return std::visit(
        [](const auto& value) -> std::size_t {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, ClientHello>) {
                return 0;
            } else if constexpr (std::is_same_v<Value, ServerWelcome>) {
                return kServerWelcomePayloadBytes;
            } else if constexpr (std::is_same_v<Value, InputPacket>) {
                return kInputPacketPrefixBytes + (value.samples.size() * kInputSampleBytes);
            } else {
                return kFullSnapshotHeaderBytes + (value.entities.size() * kEntityStateBytes);
            }
        },
        message);
}

void encode_payload(Writer&, const ClientHello&) {}

void encode_payload(Writer& writer, const ServerWelcome& message) {
    writer.write_u32(message.client_id.value());
    writer.write_u32(message.controlled_entity_id.value());
    writer.write_u32(message.server_tick);
}

void encode_payload(Writer& writer, const InputPacket& message) {
    writer.write_u8(static_cast<std::uint8_t>(message.samples.size()));
    writer.write_u32(message.last_received_snapshot_id.value());
    for (const auto& sample : message.samples) {
        writer.write_u32(sample.sequence_id.value());
        writer.write_u32(sample.client_tick);
        writer.write_float(sample.movement_x);
        writer.write_float(sample.movement_y);
        writer.write_u16(sample.action_bits);
    }
}

void encode_payload(Writer& writer, const FullSnapshot& message) {
    writer.write_u32(message.snapshot_id.value());
    writer.write_u32(message.server_tick);
    writer.write_u32(message.last_processed_input_id.value());
    writer.write_u8(message.pending_input_count);
    writer.write_u16(static_cast<std::uint16_t>(message.entities.size()));
    for (const auto& entity : message.entities) {
        writer.write_u32(entity.entity_id.value());
        writer.write_u8(static_cast<std::uint8_t>(entity.kind));
        writer.write_float(entity.position_x);
        writer.write_float(entity.position_y);
        writer.write_float(entity.mass);
    }
}

[[nodiscard]] DecodeResult decode_client_hello(Reader& reader) {
    if (reader.remaining() != 0) {
        return CodecError::TrailingBytes;
    }
    return Message{ClientHello{}};
}

[[nodiscard]] DecodeResult decode_server_welcome(Reader& reader) {
    std::uint32_t client_id{};
    std::uint32_t controlled_entity_id{};
    ServerWelcome message;
    if (!reader.read_u32(client_id) || !reader.read_u32(controlled_entity_id) ||
        !reader.read_u32(message.server_tick)) {
        return CodecError::Truncated;
    }
    if (reader.remaining() != 0) {
        return CodecError::TrailingBytes;
    }
    message.client_id = ClientId{client_id};
    message.controlled_entity_id = EntityId{controlled_entity_id};
    if (const auto error = validate(message)) {
        return *error;
    }
    return Message{message};
}

[[nodiscard]] DecodeResult decode_input_packet(Reader& reader) {
    std::uint8_t sample_count{};
    std::uint32_t last_received_snapshot_id{};
    if (!reader.read_u8(sample_count)) {
        return CodecError::Truncated;
    }
    if (sample_count == 0 || sample_count > kMaximumInputSamplesPerPacket) {
        return CodecError::OutOfRange;
    }
    const auto expected_remaining =
        sizeof(std::uint32_t) + (static_cast<std::size_t>(sample_count) * kInputSampleBytes);
    if (reader.remaining() < expected_remaining) {
        return CodecError::Truncated;
    }
    if (reader.remaining() > expected_remaining) {
        return CodecError::TrailingBytes;
    }

    InputPacket message;
    if (!reader.read_u32(last_received_snapshot_id)) {
        return CodecError::Truncated;
    }
    message.last_received_snapshot_id = SnapshotId{last_received_snapshot_id};
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

[[nodiscard]] DecodeResult decode_full_snapshot(Reader& reader) {
    std::uint32_t snapshot_id{};
    std::uint32_t last_processed_input_id{};
    std::uint8_t pending_input_count{};
    std::uint16_t entity_count{};
    FullSnapshot message;
    if (!reader.read_u32(snapshot_id) || !reader.read_u32(message.server_tick) ||
        !reader.read_u32(last_processed_input_id) || !reader.read_u8(pending_input_count) ||
        !reader.read_u16(entity_count)) {
        return CodecError::Truncated;
    }

    const auto expected_entity_bytes = static_cast<std::size_t>(entity_count) * kEntityStateBytes;
    if (reader.remaining() < expected_entity_bytes) {
        return CodecError::Truncated;
    }
    if (reader.remaining() > expected_entity_bytes) {
        return CodecError::TrailingBytes;
    }

    message.snapshot_id = SnapshotId{snapshot_id};
    message.last_processed_input_id = InputSequenceId{last_processed_input_id};
    message.pending_input_count = pending_input_count;
    message.entities.reserve(entity_count);
    for (std::uint16_t index = 0; index < entity_count; ++index) {
        std::uint32_t entity_id{};
        std::uint8_t entity_kind{};
        EntityState entity;
        if (!reader.read_u32(entity_id) || !reader.read_u8(entity_kind) ||
            !reader.read_float(entity.position_x) || !reader.read_float(entity.position_y) ||
            !reader.read_float(entity.mass)) {
            return CodecError::Truncated;
        }
        entity.entity_id = EntityId{entity_id};
        entity.kind = static_cast<EntityKind>(entity_kind);
        message.entities.push_back(entity);
    }

    if (const auto error = validate(message)) {
        return *error;
    }
    return Message{std::move(message)};
}

} // namespace

EncodeResult encode(const Message& message) {
    if (const auto error = validate(message)) {
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
    }
    return CodecError::UnknownMessageKind;
}

} // namespace dots::protocol

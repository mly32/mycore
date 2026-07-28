#pragma once

#include "dots/protocol/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace dots::protocol {

inline constexpr std::uint16_t kProtocolVersion = 4;
inline constexpr std::size_t kPacketHeaderBytes = 12;
inline constexpr std::size_t kMaximumEncodedInputPacketBytes = 75;
inline constexpr std::size_t kTargetTransportPayloadBytes = 1'200;
inline constexpr std::size_t kMaximumEncodedMessageBytes = std::size_t{64} * 1'024;

enum class CodecError : std::uint8_t {
    MessageTooLarge,
    Truncated,
    InvalidMagic,
    UnsupportedVersion,
    UnknownMessageKind,
    UnsupportedFlags,
    PayloadLengthMismatch,
    TrailingBytes,
    InvalidId,
    InvalidEnum,
    InvalidNumber,
    OutOfRange,
    InvalidInputOrdering,
    DuplicateEntity,
    DuplicateOwner,
    InvalidCheckpoint,
    InvalidReceiptOrdering,
    TooManyReceipts,
    TooManyEntities,
};

using EncodedMessage = std::vector<std::byte>;
using EncodeResult = std::variant<EncodedMessage, CodecError>;
using DecodeResult = std::variant<Message, CodecError>;

[[nodiscard]] std::optional<CodecError> validate(const Message& message);
[[nodiscard]] EncodeResult encode(const Message& message);
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes);

[[nodiscard]] constexpr bool fits_target_transport_payload(std::size_t encoded_size) noexcept {
    return encoded_size <= kTargetTransportPayloadBytes;
}

} // namespace dots::protocol

# Feature 08: Protocol Binary Codec

## Goal

Define Dots wire messages before adding a transport. Add a hostile-input-safe binary codec with
explicit framing, byte order, widths, validation, and packet-size policy. Keep Dots protocol
types game-owned and independent of simulation storage and network APIs.

The dependency boundary is:

```text
Dots::Protocol
  `-- MyCore::Core
```

Simulation keeps its current local IDs. Protocol-owned IDs are the stable wire domain, and the
authoritative runtime will map between the two when networking is integrated in Feature 09.

## Wire Format

Every encoded message begins with this 12-byte header:

| Field | Width | Value |
|---|---:|---|
| Magic | 4 bytes | ASCII `DOTS` |
| Protocol version | `uint16` | `1` |
| Message kind | `uint8` | `1` through `4` |
| Flags | `uint8` | `0`; other values are rejected |
| Payload length | `uint32` | Exact number of bytes after the header |

Integers and IEEE-754 binary32 float bit patterns use big-endian network byte order. Encoders
write fields individually; no C++ object representation is copied to the wire. Decoders require
an exact payload-length match and exact payload consumption.

Protocol IDs use `MyCore::Core` strong IDs with `uint32` representations and the existing
all-ones invalid sentinel. Ticks are `uint32` wire values.

### Messages

1. `ClientHello` has an empty payload. The common header carries the requested version.
2. `ServerWelcome` contains the assigned client ID, controlled protocol entity ID, and current
   server tick.
3. `InputCommand` contains an input sequence ID, client tick, two binary32 movement components,
   `uint16` action bits, and the last received snapshot ID. No action bits are defined yet, so
   the field must be zero. An invalid snapshot ID means no snapshot has been received.
4. `FullSnapshot` contains a snapshot ID, server tick, last processed input sequence ID, a
   `uint16` entity count, and fixed-width entity records. Each record contains a protocol entity
   ID, a `uint8` player-or-food kind, binary32 position components, and binary32 mass. An invalid
   input sequence means no input has been processed.

Full snapshots preserve caller-supplied entity order. Duplicate entity IDs are invalid; the
codec does not silently sort or rewrite records.

## Validation and Size Policy

Encoding and decoding apply the same semantic checks:

- Required IDs are valid; optional acknowledgements may use the invalid sentinel.
- Message kinds, entity kinds, flags, and action bits are known.
- Movement and snapshot floats are finite. Movement components are in `[-1, 1]` and squared
  magnitude is at most `1.0001`; entity mass is positive.
- Entity counts match the available bytes and entity IDs are unique.
- Truncated input, trailing bytes, unsupported versions, false lengths, and oversized input are
  rejected without reading beyond the supplied span.

Codec failures return a typed error value rather than throwing for malformed input.

`kTargetTransportPayloadBytes` is 1,200 bytes and describes the preferred complete encoded Dots
message size carried by transport. `kMaximumEncodedMessageBytes` is 64 KiB and is a hard
hostile-input safety limit. Initial full snapshots may exceed the target while remaining below the
safety limit. Fragment avoidance, prioritization, quantization, and per-client budgets remain
Feature 16 work.

## Public API and Implementation

- Add `Dots::Protocol` under `games/dots/protocol`, depending only on `MyCore::Core`.
- Define protocol-owned client, snapshot, input-sequence, and entity IDs; message value types;
  an entity-kind enum; and a `std::variant` containing all supported messages.
- Expose `encode(const Message&)` as a variant of encoded bytes or `CodecError`, and
  `decode(std::span<const std::byte>)` as a variant of `Message` or `CodecError`.
- Use one stable error enum covering size, framing, version/type/flags, truncation/trailing data,
  invalid IDs/enums/numbers/ranges, duplicate entities, and invalid in-memory messages.
- Do not add transport, socket, server-loop, client-mode, replication, compression, delta, or
  bit-packing behavior in this feature.

## Tests

- Round-trip both handshake messages, input, empty snapshots, and populated snapshots containing
  both entity kinds.
- Compare exact golden bytes for representative input and full-snapshot messages.
- Reject header truncation, bad magic, unsupported versions, unknown kinds, nonzero flags,
  mismatched lengths, trailing bytes, unknown enums, and messages over the safety cap.
- Reject invalid required IDs, unsupported actions, non-finite values, excessive movement,
  non-positive mass, duplicate entities, and inconsistent entity counts on the applicable
  encode and decode paths.
- Verify an encoded message may exceed the 1,200-byte target but not the 64-KiB safety limit.
- Keep existing simulation, presentation, client, server, bot, and engine tests passing.

## Exit Criteria

- Every Feature 08 message round-trips without a transport and representative golden bytes are
  fixed by tests.
- Malformed input returns a deterministic codec error without crashing or partial acceptance.
- `Dots::Protocol` depends only on `MyCore::Core`; engine targets expose no Dots wire types.
- Future transport code can dispatch one framed byte payload through the generic decoder.

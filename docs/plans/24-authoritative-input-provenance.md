# Feature 24 Authoritative Input Provenance

## Problem and Constraints

Dots authority schedules input by per-session sequence and arrival. A snapshot proves only that
authority processed a cumulative sequence frontier no later than the snapshot tick; it does not
show the exact receive or application tick for each input. The protocol `client_tick` is a local
sampling ordinal and cannot be subtracted from a server tick.

This feature makes the existing scheduler observable without changing protocol version 5,
command ordering, one-command-per-session scheduling, simulation, snapshots, or client
prediction. Diagnostics remain disabled by default and must have bounded memory and explicitly
bounded artifact output.

## Goals and Non-Goals

- Record the client ID, input sequence, local `client_tick`, receive-side server tick,
  authoritative application tick, and queue wait for every retained applied input.
- Preserve the first receive tick across redundant and reordered packet delivery.
- Produce a versioned JSONL trace plus rate-limited aggregate server logs.
- Keep cumulative counts and queue-wait distributions exact after the raw trace limit is reached.
- Prove that enabling diagnostics produces byte-identical snapshots and checkpoints.

This is not clock synchronization, network-latency measurement, lag compensation, server rewind,
a replicated debug stream, or the portable replay artifact. Feature 17 owns the later load-run
metrics directory and combined run-summary schema.

## Chosen Design

The server polls immediately before the authoritative tick it is preparing. A fresh input accepted
during that poll receives that upcoming tick as `receive_server_tick`. After a successful
`World::advance()`, the completed journal tick is `application_server_tick`, and
`queue_wait_ticks` is their difference. An input consumed on the first available tick therefore
waits zero ticks. Network transit before receipt is not measured.

Each pending-map entry retains its decoded sample and first receive tick. Redundant or stale
copies do not replace it. The runtime publishes a completed provenance record only after the
authoritative World commits. A bounded drain buffer decouples runtime capture from filesystem
output; buffer overflow drops the oldest undrained record and increments a visible counter
without affecting gameplay. Cumulative accepted, applied, discarded, pending, dropped, and
queue-wait statistics do not depend on retained raw records.

The server executable enables both output forms only when passed an output path and a positive
maximum applied-record count. It writes a schema-version 1 JSONL header, chronological applied
records up to that explicit limit, and a final summary on normal shutdown. After the raw limit,
it continues exact aggregation and reports omitted records. Every 300 server ticks and at normal
shutdown it logs one cumulative summary; it never logs individual inputs.

## Ownership and Interfaces

- `Dots::Server` owns provenance records, cumulative statistics, bounded retention, and stable
  JSONL serialization.
- `dots_server` owns filesystem creation, the explicit artifact limit, periodic logging, and
  write-failure policy.
- `dots_session.py` only validates and forwards the paired trace arguments. Feature 17 may later
  select their values as part of a metrics run.

`RuntimeSettings::input_provenance_record_capacity` is zero by default. A positive value enables
capture. Runtime callers drain records and inspect a cumulative summary; protocol and simulation
types gain no diagnostic fields.

## Implementation Status

Implemented on `feature/24-authoritative-input-provenance`. The macOS Debug build, all 280 host
tests, full tracked-source format check, and clang-tidy pass. A bounded native bot run with 5 ms
simulated one-way latency and 15 percent packet loss produced a complete trace with exact applied
rows and no runtime-buffer drops; a separately capped run retained one raw row, continued exact
aggregation, and reported every omitted row in its final summary.

## Validation and Exit Criteria

- Controlled two-client schedules prove exact receive, application, and zero/nonzero wait ticks,
  including commands from both clients committed in one World tick.
- Loss, redundancy, reordering, and duplicates produce one record per applied sequence and retain
  the first receive tick.
- Respawn, spectator, disconnect, failed-step, runtime-buffer, and artifact-limit behavior is
  deterministic and bounded.
- Enabled and disabled runs with the same input schedule produce identical authoritative
  checkpoints and protocol payloads.
- JSONL schema, option pairing, existing-file refusal, launcher forwarding, aggregate logs, and
  final summaries have automated coverage where practical.
- Format, clang-tidy, focused tests, full host CTest, and a bounded impaired headless trace run
  pass.

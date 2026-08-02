# Rollback Prediction Correctness Audit

## Problem and Constraints

The validation baseline exposed a real dangling participant view and a terminal prediction-scope
failure during an impaired multi-client session. The named regression suite covers the observed
paths well, but the reusable rollback kernel and its Dots boundaries still lack systematic
contract, invariant, differential, and generated-input coverage.

This work stays on `chore/validation-baseline` because the sanitizer and fuzz infrastructure is
the natural gate for the audit. It must preserve protocol version 5, Dots gameplay rules,
server authority, presentation semantics, and current single-thread ownership.

## Goals and Non-Goals

- Make scope epoch identity enforceable by the generic rollback API.
- Validate Dots prediction scopes consistently at construction, projection, restore, and step.
- Reject malformed consequence batches before handler state changes or unsafe dereferences.
- Prevent borrowed rollback and client-runtime views from escaping temporary owners.
- Add deterministic state-machine, causal-closure differential, lifecycle, and libFuzzer coverage.
- Keep expected stale authority, history gaps, and legitimate scope growth typed and recoverable.

This is not a transport, protocol, simulation-rule, presentation-policy, concurrency, or
performance feature. The fuzzer raises confidence and produces reproducible counterexamples; it
does not replace the deterministic invariant and oracle tests.

## Chosen Design

`RollbackModel::Scope` becomes equality-comparable. A hard resync may reuse an equal scope at the
current epoch, but replacing scope membership or policy requires a strictly newer epoch.

Dots exposes one complete scope validator. It checks profiles and fallback, mechanics and their
required domains, causal channels, ordered valid IDs and subset relations, replay horizon, and
the special `OwnedGameplay` contract. All public model entry points use that validator.

The consequence router prevalidates an entire batch. Transition payload shapes must be
current-only for first-predicted and authority-only, previous-and-current for revised and
confirmed, and previous-only for retracted; revised and confirmed alternatives must match. A
malformed batch returns typed contract failures without changing handlers, ledgers, or statistics.
Ordinary handler failures remain non-retried and nonfatal. A Dots graphical client treats an
impossible batch contract failure as a high-severity error and terminates only its own session.

Borrowed pointers, references, and spans remain available from lvalue owners only. Rvalue
overloads are deleted so misuse fails at compile time.

## Ownership and Data Flow

- `MyCore::Rollback` owns epoch equality, event-batch validation, observable invariants, and the
  structured rollback fuzzer.
- `Dots::Prediction` owns complete scope validation and the full-world versus interaction-closure
  differential oracle.
- `Dots::ClientRuntime` and the client composition root own defeat, terminal receipt, respawn,
  and session-failure containment tests.
- The required Linux Clang sanitizer/fuzz CI job owns bounded protocol and rollback fuzz smoke.

Natural network timing conditions follow existing typed reject, rebase, or resync paths and must
preserve committed state on failure. Structurally impossible trusted output is contract
corruption: preserve the last committed state for diagnostics, log the typed failure, and stop
the affected consumer instead of attempting recovery from potentially corrupted integration
state.

## Implementation Status

Implemented on `chore/validation-baseline`. The macOS Debug build, all 273 host tests, the full
tracked-source format check, and clang-tidy pass. A standalone ASan/UBSan build of the structured
rollback harness completes its checked-in seeds and 1,000 deterministic generated traces, and a
60-second 10-player-plus-spectator session completes at 5 ms simulated latency and 15% packet
loss. The actual libFuzzer runtime and the complete Linux ASan/UBSan suite remain required CI
checks because the development host does not provide the Linux fuzz toolchain.

## Validation and Exit Criteria

- Rollback operations satisfy tick, history, command-frontier, scope-epoch, commit-coordinate,
  event-retirement, failure-atomicity, and repeatability invariants.
- Malformed consequence batches cannot call handlers or mutate router state.
- Fixed-seed Dots scenarios produce identical scoped checkpoints and subscribed events between
  full replication and interaction closure across swept, chained, growth, split, and launch
  boundaries.
- Terminal authority before welcome, elimination with deferred input, topology growth, and
  authoritative respawn do not retain stale prediction state or duplicate receipts.
- Linux ASan/UBSan runs the complete suite; protocol fuzz smoke runs 2,000 inputs and rollback
  fuzz smoke runs 5,000 inputs with independent corpora and crash artifacts.
- Host format, clang-tidy, build, full CTest, a bounded impaired headless soak, and the reported
  split/eat/defeat/respawn scenario complete without a prediction or sanitizer failure.

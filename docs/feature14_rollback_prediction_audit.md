# Feature 14 Rollback and Prediction Audit

## Remediation Status

Implemented in Feature 14 step 6.5:

- `OwnedGameplay` is transition-closed for owner movement and split/merge lifecycle.
- Prediction state closure and owner-participant event subscriptions are separate scope fields.
- Receipts use a bounded inbox with accepted, event-batch-published, and server-retired
  frontiers; only the published frontier is acknowledged.
- Pre-welcome and Spectating transitions publish pending receipts before prediction is cleared.
- Live semantic event keys cannot be reused under another receipt sequence.
- Client initialization, advance, reconciliation, same-tick authority refresh, scope rebase, and
  hard resync preserve observable event batches instead of bypassing timeline commits.
- A follow-up command-frontier audit distinguishes successfully sent input, retained outer
  input, timeline-submitted input, and authoritative ACK. A validated ACK through an input that
  prediction deliberately deferred now selects hard resync and replays the remaining
  unacknowledged suffix instead of failing normal reconciliation.

The findings below retain their original present-tense diagnosis so the failure modes and
prevention guidance remain useful.

## Assessment

The Feature 14 rollback kernel has a sound transactional foundation: checkpoints are complete,
retained stimuli are immutable, replay occurs in scratch state, event transitions are typed, and
the existing engine and Dots tests pass. The production client also demonstrated that the same
Dots simulation can reconcile movement, food, absorption, split, and merge.

The audit nevertheless found integration contracts that must be corrected before persistent
presentation and consequence handlers are installed. The current tests emphasize state
convergence. They do not yet cover every transition between prediction profiles, session modes,
authority receipt delivery, and event subscriptions. Consequently, a client can converge to the
right checkpoint while losing, duplicating, or indefinitely retaining the event information
needed by presentation.

This is a step 6.5 remediation for Feature 14. It is not evidence that the generic rollback
timeline should be replaced. The fixes make the Dots model contract precise, route every
successful timeline transaction to an explicit event output, and separate replicated state from
authority receipt delivery.

## Findings

### 1. The owned fallback is not transition-closed

`OwnedMovement` selects only the movement mechanic, but Dots currently gates integration and
decay of an already-existing split launch velocity behind `SplitMerge`. Entering the fallback
while an owned piece is launched can therefore freeze momentum until the next authoritative
checkpoint. The mechanic table says movement owns player kinematics, while the implementation
lets a different feature gate suppress part of that state transition.

The corrected fallback is `OwnedGameplay`: movement plus owner-local split, launch, cooldown,
cohesion, and merge. Food consumption and player absorption remain contested and authoritative.
Movement always integrates and decays existing launch velocity; `SplitMerge` owns creation of new
split topology, cohesion, and merge resolution.

Prevention: every selectable mechanic needs tests for state that was created while another
mechanic was enabled. A disabled mechanic may prevent new transitions that it owns, but it must
not freeze a state field owned by an enabled mechanic.

### 2. Predicted state membership and consequence subscriptions are different sets

An interaction-closed World may include remote players so their movement, consumption, or
removal can affect an owned result. That does not mean the local consequence system subscribed to
every event those remote players produce. The server currently sends receipts only to owners who
participated in an event.

If a remote-to-remote event enters the timeline journal, prediction can create a speculative
event that this client will never receive authority to confirm. Its state transition is still
needed for correct replay, but its presentation consequence is not.

The Dots prediction scope therefore needs an explicit owner-participant event subscription.
Simulation applies every event in the state closure; the model exposes to the rollback event
lifecycle only events involving a subscribed owner. Server routing and prediction filtering must
share one participant-extraction helper.

Prevention: treat causal state, predicted entities, render interest, and consequence
subscriptions as independent contracts even when their sets happen to match in a simple game.

### 3. Receipt history is cumulative replicated state

`ReplicatedWorld` appends every accepted authority receipt to a vector. Transactional snapshot
application copies the entire `ReplicatedWorld`, so a long session repeatedly copies an
ever-growing event history even after the server has retired those receipts.

Receipts are a delivery stream, not World state. They need a bounded inbox with explicit
accepted, published, and server-retired frontiers. Snapshot application should return only the
new delta. Receipt payloads and semantic-key records can be discarded once the server and
rollback/consequence ledgers jointly prove they cannot be replayed.

Prevention: do not use a replicated snapshot object as an append-only inbox. Every sequence
stream needs an ownership boundary, retirement proof, and storage bound.

### 4. Acceptance, publication, and acknowledgement are conflated

The current input packet acknowledges the highest receipt accepted by `ReplicatedWorld`.
Acceptance only proves that bytes and sequence ordering were valid. It does not prove that the
receipt was reconciled with prediction or exposed to consequence handlers.

The client must acknowledge only the published frontier. A receipt becomes published after a
successful timeline transaction has emitted its event batch, or after an authority-only batch
has been queued when no timeline exists. Failed candidate state never advances that frontier.

Prevention: name and measure receipt frontiers by their actual guarantees. Network receipt,
semantic acceptance, local publication, consequence delivery, and server retirement are not the
same operation.

### 5. Pre-welcome and spectating receipts can be skipped

Authority installation returns early before immutable rules arrive and when the recipient is
spectating. The later size-based cursor observes that `ReplicatedWorld` already retained the
receipt and no longer considers it new. The terminal `PlayerAbsorbed` receipt is particularly
important: the snapshot that confirms defeat also changes the session to spectating, so the
receipt intended to drive a `ConfirmOnce` cue is currently bypassed before prediction is cleared.

Pending receipts must survive pre-welcome state. A transition to spectating must first hard
resync the prior timeline with the confirmed event, publish that batch, and then clear
prediction. A client with no active timeline publishes an authority-only batch instead.

Prevention: test event delivery at every session-mode boundary, including the event that causes
the boundary.

### 6. A semantic key can be reused under another receipt sequence

Replication rejects a conflicting retransmission under the same receipt sequence, but it does
not reject a later sequence containing the same stable simulation event key. One occurrence must
have one receipt identity. Otherwise a producer bug can alias two authority facts and undermine
`ConfirmOnce` and cancelable-consequence ledgers.

The receipt inbox must reject any new sequence that reuses a live semantic key. Same-sequence,
same-payload retransmission remains valid. The authoritative simulation contract continues to
guarantee that entity- and input-derived event keys are never reused after retirement.

Prevention: test transport sequence identity and semantic occurrence identity independently.

### 7. The client bypasses transactional scope rebase and discards commits

The engine already provides `rebase_scope_with_stimulus_refresh`, which compares the old and new
predicted event indexes and emits one atomic `ScopeRebase` commit. The client instead initializes
a new timeline and manually replays inputs when closure membership changes. That is sufficient
for the current checkpoint projection, but it loses old event lifecycle information. Emitting
the scratch commits later would incorrectly replay first-occurrence consequences; discarding
them loses consequences entirely.

The client must use the kernel rebase transaction and retain event batches from every committed
initialize, advance, reconcile, rebase, and hard-resync operation. Dots exposes those batches
through a bounded pull API for the presentation/consequence layer.

Prevention: a composition root may move or summarize a successful kernel commit, but it must not
reimplement a kernel transaction or ignore its durable outputs.

### 8. The session input frontier can advance beyond the prediction timeline

Predicted absorption of the final local piece deliberately leaves the confirmed session in
`Playing` and continues sampling and sending input. The outer client retains those inputs, but
the prediction timeline cannot step them while its speculative World has no owned player, so its
last-submitted frontier stops. A later coherent server snapshot can acknowledge one of those
deferred inputs while confirming that the player actually survived. Normal reconciliation
correctly rejects that ACK because it is beyond timeline history; the client previously treated
the expected rejection as a fatal session error.

The client now compares four explicit frontiers before choosing a timeline operation:
successfully sent, retained by the outer input ring, submitted to the timeline, and
authoritatively acknowledged. If the session-level ACK is valid, is ahead of timeline
submission, and the exact acknowledged command remains in the outer ring, the client uses the
kernel's documented hard-resync exception. It restores the validated checkpoint, advances the
timeline frontier through the ACK, discards the acknowledged outer prefix, and replays any newer
retained inputs. An ACK beyond both the timeline and retained ring remains fatal.

Prevention: any integration that can intentionally defer simulation after accepting external
commands must test the full transition matrix: authority behind the deferred range, authority
inside it, authority through all of it, and an unacknowledged suffix that must roll forward.
Do not use one generic “input history” count to represent all four frontiers.

## Larger Bug Classes

The findings share four broader failure modes:

1. Feature gates that own phases instead of explicit state transitions.
2. A single “relevance” set being reused for simulation, networking, presentation, and effects.
3. Monotonic streams without separate accepted, delivered, and retired frontiers.
4. Transactional core APIs whose commit records are discarded at an integration boundary.
5. Outer command acceptance and inner simulation submission being treated as one frontier even
   when the model can deliberately defer stepping.

Reviews of future predicted mechanics should trace each state field and event from authoritative
creation through checkpointing, replay, subscription, publication, consequence retirement, and
debug visibility. State convergence alone is not sufficient acceptance evidence.

## Remediation Exit Criteria

- Owned fallback remains responsive and deterministic through active split launch and merge.
- Prediction emits only owner-subscribed events while retaining all required causal state.
- Every successful client timeline transaction produces a bounded, observable event batch.
- Defeat, pre-welcome, and authority-only receipts are published exactly once before ACK.
- Receipt payload and key storage remains bounded by explicit retirement.
- Scope rebase preserves event lifecycle relative to the previously committed timeline.
- A validated ACK inside the outer retained/deferred range hard-resyncs atomically and replays
  the unacknowledged suffix; an uncovered ACK gap still fails with all frontiers logged.
- Focused hostile tests, the full host test preset, clang-format, clang-tidy, packaging, and the
  two-bot impairment session pass before Feature 14 step 7 begins.

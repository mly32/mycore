# Authoritative Spawn Search

## Status and Relationship to the Roadmap

Stages A and B were implemented as a Feature 13 follow-up before Feature 14 complete World
checkpoint work. The implemented gameplay contract is the active-count indexed square-ring search
described in `../dots_gameplay.md`.

The work stays in Dots simulation. It does not introduce an engine spawning service, client spawn
authority, a fixed player limit, or a general-purpose placement solver.

## Problem

The authoritative server must place joining and respawning players without touching or
overlapping any live player. The previous origin-first search was correct and deterministic, but
its cumulative cost grew quadratically when many initial-size players were inserted into a fresh
World.

Every call to the previous `spawn_player_safely` restarted at the origin and walked the same
12-world-unit square spiral. With the first `n` spiral points occupied, the next spawn rechecked
those `n` points before finding point `n + 1`. Creating `N` players therefore performed:

```text
1 + 2 + ... + N = N(N + 1) / 2
```

candidate classifications. The deterministic 1,000-player test builds two Worlds, so it performs
1,001,000 classifications, despite creating only 2,000 players.

Each classification previously calculated the candidate's spatial-grid range more than once and
used the generic `SpatialGrid::query` path. That query creates a result vector and an
`unordered_set`, looks up one or more cells in an ordered map, and deduplicates entities even
though spawn placement only needs to know whether one blocking player exists.

The final exhaustive test audit also compares 499,500 player pairs, but sampling shows that the
repeated spawn search is the dominant cost.

## Profiling Baseline

On the macOS development host on 2026-07-23, running only the Catch2 test executable produced:

| Build | Wall time | User CPU | System CPU |
| --- | ---: | ---: | ---: |
| Clang Debug | 2.66 s | 2.65 s | 0.00 s |
| Clang Release | 0.25 s | 0.25 s | 0.00 s |

The Debug sample stacks were dominated by:

```text
spawn_player_safely
  World::is_initial_player_spawn_clear
    SpatialGrid::query
      ordered-map cell lookup and comparison
      unordered-set insertion and deduplication
```

The large Debug-to-Release difference shows that unoptimized container and abstraction overhead
amplifies the repeated search, but it does not remove the underlying quadratic candidate count.
Wall times are diagnostic evidence, not portable test thresholds.

The focused timing command is:

```bash
cd build/<preset>/tests
/usr/bin/time -l \
    ../bin/dots_simulation_tests \
    "Safe player spawning follows an unbounded deterministic spiral"
```

On macOS, a focused sample can be captured by launching that test, obtaining its PID, and running:

```bash
/usr/bin/sample <pid> 2 -file /tmp/mycore_spawn_profile.txt
```

After Stages A and B, the same 1,000-player/two-World Debug test, now with 1,000 additional
ordinal-position assertions, completes in 0.06--0.07 seconds of wall time and 0.06 seconds of user
CPU time. That is roughly a 40-times wall-time reduction from the Debug baseline. The Release run
reports below the timing tool's 0.01-second display resolution.

## Required Behavior

Any replacement must preserve these invariants:

- The authoritative server alone selects the position and creates the player.
- The client does not propose, reserve, or predict an authoritative join or respawn position.
- Every candidate is checked against the current positions and live radii of all players.
- Touching counts as blocked, matching the simulation's circle-overlap rule.
- Food does not block a player spawn.
- Selection is deterministic for the same World state and deterministic request order.
- The search has no fixed table of spawn slots or simultaneous-player ceiling.
- Entity-ID exhaustion and absence of a representable safe position remain distinct failures.
- The result stays independent of SDL, rendering, transport, and presentation.

For Feature 14, placement must also replay identically after checkpoint restore. A mutable cursor,
free list, or random generator would become checkpoint state. A search derived entirely from the
current World avoids that additional rollback contract.

Choosing the earliest clear point from the origin is not a required gameplay rule for the
replacement. Compact placement and exact collision safety are required; origin-first hole reuse
is not.

## Considered Approaches

### 1. Preserve the search and add an early-exit spatial query

Add a spatial-grid operation that visits broad-phase candidates and stops when a caller predicate
finds a blocking player. It should:

- Validate and calculate the cell range once for each candidate.
- Avoid creating a result vector and `unordered_set`.
- Stop on the first exact player-circle overlap.
- Permit duplicate broad-phase visits when harmless, or use caller-owned reusable scratch state
  if profiling proves deduplication is still necessary.

This is the safest first optimization because it preserves every current spawn position. It
reduces constants but retains the quadratic origin restart.

### 2. Start an indexed compact sequence from the active player count

Represent the compact square-ring lattice as a direct ordinal-to-coordinate mapping:

```text
ordinal 0 -> origin
ordinal 1..8 -> first ring in a fixed order
ordinal 9..24 -> second ring
...
```

Use integer ring, edge, and offset calculations rather than walking from ordinal zero. For a
World with `n` live players, begin at ordinal `n` and continue forward until exact collision
testing finds a clear point.

For an untouched batch of equal initial-size players, ordinal `n` is the next free point. Creating
`N` players therefore performs approximately `N` classifications rather than `N(N + 1) / 2`.
The sequence can match the current square spiral, preserving the common-case positions.

Movement, removal, or growth may make ordinal `n` blocked or leave a lower hole. The search still
checks subsequent candidates exactly and remains safe, but it does not promise to fill the
earliest hole. Starting from the active count prevents placement from drifting outward based on
the lifetime number of respawns.

This approach needs no mutable cursor or random state. Its starting ordinal and result are derived
from the current World, which makes it the best fit for Feature 14 checkpoint and replay.

### 3. Store a high-water cursor

Remember the ordinal after the last successful spawn and continue from it. This makes batch
insertion inexpensive, but movement and removal leave reusable holes behind the cursor. Long-lived
servers can drift outward according to lifetime spawns rather than active population.

The cursor also becomes authoritative World state that must be serialized, checkpointed,
restored, validated, and replayed. A free-list extension can recover holes, but arbitrary player
movement and radius growth make that list expensive to maintain correctly.

This is not recommended while the state-derived active-count start provides most of the expected
benefit.

### 4. Deterministic hash or pseudo-random sampling

Hashing an owner, entity, or request identity can give different sessions different starting
points and reduce collisions at low density. A deterministic PRNG with expanding bounds offers a
similar distribution.

These approaches complicate locality, fairness, reproducibility, and failure analysis. Hashing
into a large domain can scatter players far apart; hashing into a small domain reintroduces probe
collisions. A PRNG seed and stream position become protocol or checkpoint state unless every
sample is derived statelessly.

They are not justified for the current compact Dots field.

### 5. Configured spawn zones or fixed spawn points

Spawn zones are useful future match-design tools and may eventually be script-configurable. A
finite point list alone reintroduces a player ceiling, while dynamic zones still need a
deterministic collision-safe fallback.

This should remain a later Dots gameplay rule rather than the core performance fix.

### 6. Largest-empty-region placement

Selecting the largest gap or maximizing distance from live players can improve perceived
fairness, but it requires a more expensive global search and establishes a new gameplay policy.
There is no current rule or measured need that warrants that complexity.

## Implemented Design

The improvement has two independently testable stages.

### Stage A: Allocation-free collision classification

The Dots spatial grid provides a traversal contract that terminates on the first matching entity.
`World` retains the gameplay knowledge needed to ignore food and compare the initial player circle
against live player circles.

The separate `can_index_initial_player` and `is_initial_player_spawn_clear` sequence is replaced
with one classification result:

```text
Clear
Blocked
OutsideRepresentableGrid
```

The classification computes the broad-phase cell range once. A successful spawn still lets
`World::spawn_player` validate insertion independently so its public safety contract is not
weakened.

### Stage B: Active-count indexed square rings

The compact 12-world-unit lattice order is directly indexable. Each search begins at
`world.player_count()` and advances ordinals from there.

Every ordinal remains subject to Stage A's exact live-radius collision classification. The
starting hint can never make an unsafe candidate valid; it only avoids rechecking positions that
are normally occupied in a compact initial population.

This changes one observable edge behavior: after movement, growth, or removal, the replacement
may choose a later clear point instead of the earliest clear point from the origin. That tradeoff
is acceptable because origin-first reuse is not a required game rule, and active-count seeding
keeps placement proportional to the current population.

The implementation remains Dots-owned. Do not extract an engine facility until another game has
the same placement contract.

## Important Data Flow

```text
authoritative join or eligible respawn
  -> derive start ordinal from live player count
  -> map ordinal to compact lattice coordinate
  -> classify candidate through the spatial grid
       -> reject invalid spatial bounds
       -> visit nearby entity IDs
       -> ignore food
       -> test exact live player circles
       -> stop at first touch/overlap
  -> create player at first clear candidate
  -> replicate authoritative entity and position
```

Multiple requests in one server update use the server's existing deterministic session/input
order. Each successful creation increments the live player count before the next request is
resolved.

## Validation

Correctness tests must cover:

- Golden ordinal-to-coordinate values across ring and edge boundaries.
- Repeatability across two Worlds receiving identical spawn, movement, growth, removal, and
  respawn operations.
- No touching or overlap for 1,000 initial-size players.
- Exact blocking by a grown player's live radius.
- Safe placement when movement or removal creates lower-order holes.
- Food at a candidate without blocking the spawn.
- Explicit entity-ID exhaustion and representable-grid failure.
- Deterministic request ordering when multiple sessions join or respawn together.

Performance validation should use deterministic work counts rather than flaky wall-clock
assertions:

- A fresh batch of 1,000 initial-size players should require one successful candidate
  classification per spawn in the normal case.
- The two-World repeatability test should perform about 2,000 classifications instead of the
  previous 1,001,000.
- A focused benchmark or profiler run should confirm that spatial-query allocation and ordered
  cell comparison are no longer dominant.

Feature 14 checkpoint tests must prove that restoring the same World and replaying the same
request order selects the same positions. If implementation introduces mutable search state
despite this plan, that state must be added to the complete checkpoint before Feature 14 can be
approved.

## Exit Criteria

- Joining and respawning remain server-authoritative, deterministic, and collision-safe against
  current player radii.
- The normal fresh-World batch path performs linear candidate classifications.
- Candidate collision traversal allocates no per-query result containers and exits on the first
  blocker.
- The search retains no fixed player ceiling and reports distinct exhaustion/failure causes.
- Canonical gameplay documentation describes the implemented selection policy.
- Feature 14 either relies on the state-derived search or explicitly checkpoints every added
  piece of spawn-search state.

## Unresolved Follow-ups

- Whether later match rules should prefer team zones, distance from a killer, or configurable
  spawn regions.
- Whether profiling after Stage A justifies replacing the ordered cell map; spawn search alone is
  insufficient reason to change the grid's determinism or iteration contracts.
- Whether a bounded lower-hole probe improves long-running match compactness enough to justify
  its additional policy and tests.

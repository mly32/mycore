# Features 02–04: Core Primitives Through Spatial Collision

## Goal

Build the first platform-free Dots gameplay slice: reusable engine primitives, a deterministic
fixed-step world, and Dots-owned food collision and eating rules. Keep rendering, SDL,
networking, and generic physics out of the simulation.

## Feature 02: Core, Math, and Time

- Added generic strong IDs to `MyCore::Core`.
- Added `MyCore::Math` with the `Vector2` operations required by Dots movement.
- Added `MyCore::Time` with ticks, tick-duration conversion, and a policy-free fixed-step
  accumulator.
- Kept concrete entity IDs, the 30 Hz tick rate, and gameplay policy in Dots.

## Feature 03: Fixed-Step Simulation

- Added Dots-owned entity and input-command IDs.
- Added a headless `World` with dense structure-of-arrays storage for player state.
- Added player spawning, removal, validated input sequencing, normalized movement, and 30 Hz
  stepping.
- Added deterministic input replay coverage.

## Feature 04: Spatial Grid and Eating

- Added a Dots-owned uniform spatial grid. A circle is registered in every fixed-size cell
  touched by its bounds; queries combine and deduplicate IDs from the touched cells.
- Used the grid only as a broad phase. Candidate circles still pass an exact circle-overlap
  test before gameplay rules run.
- Added food entities, player mass, `radius = sqrt(mass)`, and food consumption.
- Added an ID-indexed entity-location table so the world remains authoritative while dense
  player and food arrays support efficient iteration and swap-removal.
- Made spawning return `std::optional<EntityId>` so invalid geometry is rejected without
  consuming an ID.
- Split each tick into movement, grid update, collision gathering, deterministic resolution,
  and growth phases. The lowest player entity ID wins contested food, and growth affects
  collisions beginning on the following tick.

## Validation

- Strong-ID, vector, tick, accumulator, movement, and replay tests.
- Grid insertion, same- and cross-cell movement, removal, negative coordinates, multi-cell
  deduplication, and invalid-range tests.
- Exact collision edge cases, broad-phase false positives, food consumption, contested food,
  growth timing, and moving-food-scenario replay tests.
- The macOS debug preset builds successfully and all 20 discovered tests pass through CTest.

## Result

Dots now has an offline, deterministic Agar.io-like simulation that moves players, indexes
nearby entities, performs exact circle checks, and consumes food without depending on platform,
rendering, transport, or scripting libraries.

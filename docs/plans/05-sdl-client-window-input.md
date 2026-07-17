# Feature 05: SDL Client Window, Input, and Configuration

## Goal

Deliver the first playable offline Dots client while keeping platform input game-neutral and
the temporary SDL 2D presentation client-owned.

## Implementation

1. Add `MyCore::PlatformSDL` with RAII video initialization, move-only window ownership,
   game-neutral keyboard/mouse snapshots, and event/state polling.
2. Add pure Dots client control mapping for mouse, keyboard, and hybrid modes, then run the
   local world at 30 Hz through `FixedStepAccumulator` with bounded frame catch-up.
3. Create a hidden SDL window, initialize the temporary client renderer and vsync, show the
   window, and draw the followed player, live food, and a configurable grid with high-DPI
   mouse/output conversion.
4. Load strict TOML configuration from defaults, an optional current-directory
   `dots-client.toml`, or an explicit `--config` path. Reject unknown or invalid settings.
5. Cover snapshots, control construction, configuration, and dummy-video startup with tests;
   document normal, configured, and headless-smoke runs.

The renderer remains outside `MyCore::PlatformSDL`; Feature 06 will replace this temporary
path with `MyCore::Render`. SDL3 is target-scoped through engine-owned client modules, so
graphical game clients opt in while simulations, servers, and headless bots remain SDL-free.

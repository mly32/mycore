# Validation Baseline Before Scale Work

## Problem and Constraints

MyCore is implemented through Feature 14, but two validation commitments from the technology
plan are still missing: sanitizer presets/CI and a coverage-guided protocol decoder fuzzer.
Features 15 and 16 will materially change hostile-input and replication boundaries, so those
checks must exist before scale work begins.

The branch must remain focused on correctness tooling. Performance benchmarks belong to Feature
17, Linux server packaging remains later release work, and ThreadSanitizer remains conditional on
the introduction of project-owned concurrent execution.

## Goals and Non-Goals

- Add a reproducible Linux Clang AddressSanitizer plus UndefinedBehaviorSanitizer workflow.
- Run the complete test suite under those sanitizers in required CI.
- Add a libFuzzer target that drives arbitrary bytes through `Dots::Protocol::decode`.
- Seed the fuzzer with representative framing fragments and a protocol-aware dictionary.
- Document exact local commands and keep ordinary Debug/Release presets unchanged.

This work does not change runtime behavior, protocol version 5, simulation policy, snapshot
representation, CI packaging, or load-test metrics. It does not add an asset fuzzer because the
current asset facility reads opaque bytes rather than decoding a hostile structured format.

## Chosen Design

Add target-scoped sanitizer flags through `mycore::project_options`, enabled only by
`MYCORE_ENABLE_SANITIZERS`. The checked-in `linux-clang-asan` configure/build/test presets enable
AddressSanitizer and UndefinedBehaviorSanitizer together with frame pointers. Unsupported
compiler/platform combinations fail during configuration rather than silently producing an
unsanitized build.

Add `MYCORE_BUILD_FUZZERS` and a separate `linux-clang-fuzz` preset. The protocol library receives
libFuzzer coverage instrumentation only in that build, while the fuzzer executable supplies the
libFuzzer entry point/runtime and links the real `Dots::Protocol` target. The regular build and
test graph does not contain the fuzzer.

CI uses the sanitizer preset for a full build and CTest run. It then configures the fuzz preset,
builds only `dots_protocol_decode_fuzzer`, and performs a bounded deterministic smoke run against
the checked-in seed corpus and dictionary. Longer campaigns remain scheduled or local work.

## Ownership and Interfaces

- `cmake/ProjectOptions.cmake` owns target-scoped sanitizer/fuzzer compiler policy.
- `CMakePresets.json` owns reproducible Linux sanitizer and fuzz workflows.
- `games/dots/protocol/fuzz/` owns the Dots decoder harness and seed inputs.
- `.github/workflows/ci.yml` owns the required sanitizer and bounded fuzz jobs.

No public C++ API or wire interface changes.

## Implementation Status

Implemented on `chore/validation-baseline`. Normal macOS Debug configuration and the complete
257-test host suite pass. A direct 2,000-run ASan/UBSan/libFuzzer smoke passes with the real
decoder and checked-in dictionary. The required Linux preset executions remain the CI merge gate
because the development host is macOS.

## Validation and Exit Criteria

- Normal host Debug configuration, build, and CTest remain green.
- `linux-clang-asan` configures, builds, and runs all tests under ASan/UBSan in CI.
- `linux-clang-fuzz` builds the decoder fuzzer with Clang/libFuzzer.
- The bounded fuzz smoke run completes without a crash, sanitizer finding, timeout, or corpus
  write into the source tree.
- Invalid configuration requests fail with an actionable message.
- Build instructions and the development roadmap identify the validation branch as the gate
  before Features 24, 17, 15, and 16.

# Validation Baseline Before Scale Work

## Problem and Constraints

MyCore is implemented through Feature 14, but its validation gate still needs systematic
sanitizer, hostile-input, and rollback-kernel correctness coverage.
Features 15 and 16 will materially change hostile-input and replication boundaries, so those
checks must exist before scale work begins.

The branch must remain focused on correctness tooling. Performance benchmarks belong to Feature
17, Linux server packaging remains later release work, and ThreadSanitizer remains conditional on
the introduction of project-owned concurrent execution.

## Goals and Non-Goals

- Add a reproducible Linux Clang AddressSanitizer plus UndefinedBehaviorSanitizer workflow.
- Run the complete test suite under those sanitizers in required CI.
- Add libFuzzer targets for arbitrary protocol bytes and structured rollback operations.
- Seed the fuzzer with representative framing fragments and a protocol-aware dictionary.
- Document exact local commands and keep ordinary Debug/Release presets unchanged.

Harden rollback scope identity, consequence-batch validation, Dots prediction-scope validation,
and borrowed-view lifetimes without changing protocol version 5, simulation policy, snapshot
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
builds the protocol and structured rollback fuzzers, and performs bounded deterministic smoke
runs against checked-in seed corpora. Longer campaigns remain scheduled or local work.

The vcpkg GameNetworkingSockets archive does not expose the RTTI metadata required by Clang's
`vptr` sanitizer for its polymorphic C++ interfaces. Disable only `vptr` instrumentation in the
native GameNetworkingSockets adapter translation unit; AddressSanitizer and all other
UndefinedBehaviorSanitizer checks remain enabled there, and project-owned translation units keep
the complete sanitizer set.

## Ownership and Interfaces

- `cmake/ProjectOptions.cmake` owns target-scoped sanitizer/fuzzer compiler policy.
- `CMakePresets.json` owns reproducible Linux sanitizer and fuzz workflows.
- `games/dots/protocol/fuzz/` owns the Dots decoder harness and seed inputs.
- `engine/rollback/fuzz/` owns the structured timeline harness and seed inputs.
- `.github/workflows/ci.yml` owns the required sanitizer and bounded fuzz jobs.

The rollback model concept requires equality-comparable scopes, consequence dispatch reports gain
typed batch-contract failures, and Dots exposes its complete prediction-scope validator. Wire
interfaces remain unchanged.

## Implementation Status

Implemented on `chore/validation-baseline`. Normal macOS Debug configuration and the complete
269-test host suite pass. A direct 2,000-run ASan/UBSan/libFuzzer smoke passes with the real
decoder and checked-in dictionary. The first Linux CI execution exposed the packaged
GameNetworkingSockets RTTI boundary described above; the adapter now has a source-scoped `vptr`
exception. The follow-up audit in
[`rollback-correctness-audit.md`](rollback-correctness-audit.md) is implemented, including the
structured rollback fuzzer and deterministic Dots prediction oracle. The required Linux preset
executions remain the CI merge gate because the development host is macOS.

## Validation and Exit Criteria

- Normal host Debug configuration, build, and CTest remain green.
- `linux-clang-asan` configures, builds, and runs all tests under ASan/UBSan in CI.
- `linux-clang-fuzz` builds both fuzzers with Clang/libFuzzer.
- The bounded fuzz smoke run completes without a crash, sanitizer finding, timeout, or corpus
  write into the source tree.
- Invalid configuration requests fail with an actionable message.
- Build instructions and the development roadmap identify the validation branch as the gate
  before Features 24, 17, 15, and 16.

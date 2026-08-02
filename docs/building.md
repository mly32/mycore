# Building MyCore

## Prerequisites

- CMake 3.25 or newer
- Ninja
- A C++20 compiler: AppleClang/Clang, GCC, or MSVC
- vcpkg with `VCPKG_ROOT` set to the directory containing its `scripts/` folder
- Python 3 for tests that exercise the Dots session launcher
- `pkg-config` on macOS and Linux
- A Metal-, Vulkan-, or D3D12-capable GPU driver to run the graphical client

On macOS, install the command-line dependencies with Homebrew and the compiler through Xcode:

```bash
brew install cmake ninja vcpkg pkg-config
xcode-select --install
```

On Debian or Ubuntu, install the compiler tools and SDL's Linux backend dependencies:

```bash
sudo apt-get update
sudo apt-get install --yes \
    build-essential clang cmake ninja-build pkg-config \
    autoconf autoconf-archive automake libtool libltdl-dev \
    libx11-dev libxft-dev libxext-dev \
    libxcursor-dev libxfixes-dev libxi-dev libxrandr-dev \
    libxss-dev libxtst-dev \
    libwayland-dev libxkbcommon-dev libegl1-mesa-dev \
    libibus-1.0-dev
```

vcpkg manifest mode installs the libraries declared in `vcpkg.json`. Unix ports may still use
the host tools and system development packages listed above.

CI runs static analysis with `clang-tidy` 22 from the
[official LLVM Debian/Ubuntu packages](https://apt.llvm.org/). Use the same major version when
reproducing a Linux diagnostic locally; analyzer results can differ between LLVM releases. The
CI command is `run-clang-tidy-22 -p build/linux-clang-debug -warnings-as-errors='*' -quiet`.

## Configure, build, and test

List the configure presets available on the current host:

```bash
cmake --list-presets
```

Then configure, build, and test with the matching preset:

```bash
cmake --preset macos-clang-debug
cmake --build --preset macos-clang-debug
ctest --preset macos-clang-debug
```

Each supported platform has native Debug and Release presets:

- `macos-clang-debug` and `macos-clang-release`
- `linux-clang-debug` and `linux-clang-release`
- `windows-msvc-debug` and `windows-msvc-release`

Linux also provides validation-only presets:

- `linux-clang-asan` builds and tests the complete project with AddressSanitizer and
  UndefinedBehaviorSanitizer.
- `linux-clang-fuzz` builds the coverage-guided Dots protocol decoder and structured rollback
  timeline fuzzers.

Release presets enable the compiler's optimized `Release` configuration and define `NDEBUG`.
Tests remain enabled so optimized builds can be validated with the matching test preset.

The Windows presets allow vcpkg to select its default dynamic-library triplet. CMake stages the
resulting runtime DLLs beside executables and includes them in the packaged client. macOS and
Linux allow vcpkg to infer their native configuration as well. The presets do not hard-code a
target architecture; the selected compiler environment and vcpkg triplet determine it.

Build a focused target during iteration:

```bash
cmake --build --preset macos-clang-debug --target dots_client
```

Build and run the deterministic optimized rollback workload with:

```bash
cmake --preset macos-clang-release
cmake --build --preset macos-clang-release --target dots_rollback_workload
./build/macos-clang-release/bin/dots_rollback_workload --iterations 1000
```

The workload prints CSV measurements but makes no timing assertion in CTest. The
[Feature 14 workload record](feature14_rollback_workload_results.md) defines its scenario,
current baseline, and replay-scheduler decision threshold.

Use a fresh cache after changing compilers, architectures, or vcpkg triplets:

```bash
cmake --fresh --preset macos-clang-debug
```

## Sanitizers and fuzzing

Run the complete Linux sanitizer build with:

```bash
cmake --preset linux-clang-asan
cmake --build --preset linux-clang-asan
ctest --preset linux-clang-asan
```

Build both fuzzers and copy their source seeds to writable build directories before running them.
libFuzzer may minimize or extend a working corpus; it must not write into a tracked seed
directory. The structured rollback grammar is byte-oriented and does not require a dictionary.

```bash
cmake --preset linux-clang-fuzz
cmake --build --preset linux-clang-fuzz
cmake -E copy_directory \
    games/dots/protocol/fuzz/corpus \
    build/linux-clang-fuzz/protocol-fuzz-corpus
cmake -E copy_directory \
    engine/rollback/fuzz/corpus \
    build/linux-clang-fuzz/rollback-fuzz-corpus
./build/linux-clang-fuzz/bin/dots_protocol_decode_fuzzer \
    build/linux-clang-fuzz/protocol-fuzz-corpus \
    -dict=games/dots/protocol/fuzz/protocol.dict \
    -max_len=65536 \
    -runs=2000
./build/linux-clang-fuzz/bin/mycore_rollback_timeline_fuzzer \
    build/linux-clang-fuzz/rollback-fuzz-corpus \
    -max_len=4096 \
    -runs=5000
```

These presets intentionally require Clang on Linux. ThreadSanitizer remains deferred until the
project introduces its own concurrent execution, and performance benchmarks remain Feature 17
work.

## Package the Dots client

Build and verify a relocatable client archive with:

```bash
cmake --preset macos-clang-release
cmake --build --preset macos-clang-release --target dots_client_package
```

Packages and SHA-256 checksums are written under `build/<preset>/packages/`. Windows produces a
ZIP file; macOS and Linux produce `.tar.gz` archives. The target extracts the archive and runs a
headless package smoke check that verifies the executable, configuration, schema, and shaders
without creating a GPU device.

## Visual Studio Code

Open the repository root, install the recommended workspace extensions, and select the matching
configure and build presets through the CMake Tools commands. Choose `dots_client`,
`dots_server`, or `dots_bot` as the launch target before running or debugging.

Shared editor settings live under `.vscode/`. Put machine-local CMake overrides in the ignored
`CMakeUserPresets.json`.

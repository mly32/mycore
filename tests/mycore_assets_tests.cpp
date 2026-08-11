#include "mycore/assets/directory_source.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(_WIN32)
#    include <process.h>
#else
#    include <unistd.h>
#endif

namespace {

[[nodiscard]] auto current_process_id() noexcept {
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}

class TemporaryAssetDirectory {
public:
    TemporaryAssetDirectory() {
        constexpr auto kMaximumAttempts = std::size_t{100};
        const auto temporary_root = std::filesystem::temp_directory_path();
        for (auto attempt = std::size_t{}; attempt < kMaximumAttempts; ++attempt) {
            // Catch registers each case as an independent CTest process. Under --parallel, a
            // process-local counter alone gives every case the same directory and lets one
            // destructor remove another case's fixture. The process ID separates concurrent
            // cases; atomic create-and-retry also handles stale directories after PID reuse.
            auto candidate =
                temporary_root / ("mycore-assets-tests-" + std::to_string(current_process_id()) +
                                  "-" + std::to_string(counter_++));
            std::error_code error;
            if (!std::filesystem::create_directory(candidate, error)) {
                if (error) {
                    throw std::filesystem::filesystem_error{
                        "Could not create temporary asset test directory", candidate, error};
                }
                continue;
            }

            path_ = std::move(candidate);
            if (!std::filesystem::create_directory(path_ / "nested", error)) {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
                throw std::filesystem::filesystem_error{
                    "Could not create nested temporary asset test directory",
                    path_ / "nested",
                    error};
            }
            return;
        }
        throw std::runtime_error{"Could not allocate a unique temporary asset test directory"};
    }

    ~TemporaryAssetDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryAssetDirectory(const TemporaryAssetDirectory&) = delete;
    TemporaryAssetDirectory& operator=(const TemporaryAssetDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    inline static std::size_t counter_{};
    std::filesystem::path path_;
};

void write_bytes(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream stream{path, std::ios::binary};
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

} // namespace

TEST_CASE("Directory asset source reads rooted binary files", "[assets]") {
    TemporaryAssetDirectory directory;
    write_bytes(directory.path() / "nested" / "shader.bin", std::string{"a\0b", 3});
    write_bytes(directory.path() / "empty.bin", {});

    const mycore::assets::DirectorySource assets{directory.path()};
    const auto bytes = assets.read("nested/shader.bin");

    REQUIRE(bytes.size() == 3);
    REQUIRE(bytes[0] == std::byte{'a'});
    REQUIRE(bytes[1] == std::byte{});
    REQUIRE(bytes[2] == std::byte{'b'});
    REQUIRE(assets.read("empty.bin").empty());
    REQUIRE(assets.root().is_absolute());
}

TEST_CASE("Directory asset source reports missing files", "[assets]") {
    TemporaryAssetDirectory directory;
    const mycore::assets::DirectorySource assets{directory.path()};

    REQUIRE_THROWS_WITH(assets.read("missing.bin"),
                        Catch::Matchers::ContainsSubstring("missing.bin"));
}

TEST_CASE("Directory asset source rejects paths outside its root", "[assets]") {
    TemporaryAssetDirectory directory;
    const mycore::assets::DirectorySource assets{directory.path()};

    REQUIRE_THROWS_AS(assets.read("../outside.bin"), mycore::assets::Error);
    REQUIRE_THROWS_AS(assets.read(std::filesystem::temp_directory_path() / "outside.bin"),
                      mycore::assets::Error);
    REQUIRE_THROWS_AS(assets.read({}), mycore::assets::Error);
}

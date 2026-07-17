#include "mycore/assets/directory_source.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class TemporaryAssetDirectory {
public:
    TemporaryAssetDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("mycore-assets-tests-" + std::to_string(counter_++))) {
        std::filesystem::create_directories(path_ / "nested");
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

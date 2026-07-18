#pragma once

#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace mycore::assets {

class Error : public std::runtime_error {
public:
    explicit Error(const std::string& message);
};

using Bytes = std::vector<std::byte>;

class DirectorySource {
public:
    explicit DirectorySource(const std::filesystem::path& root);

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] Bytes read(const std::filesystem::path& asset_name) const;

private:
    [[nodiscard]] std::filesystem::path resolve(const std::filesystem::path& asset_name) const;

    std::filesystem::path root_;
};

} // namespace mycore::assets

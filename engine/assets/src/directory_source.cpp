#include "mycore/assets/directory_source.hpp"

#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

namespace mycore::assets {
namespace {

[[nodiscard]] bool is_within(const std::filesystem::path& root,
                             const std::filesystem::path& candidate) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() || *candidate_part != *root_part) {
            return false;
        }
    }
    return true;
}

[[noreturn]] void throw_path_error(const std::filesystem::path& path, const std::string& detail) {
    throw Error{"Asset '" + path.string() + "': " + detail};
}

} // namespace

Error::Error(const std::string& message)
    : std::runtime_error(message) {}

DirectorySource::DirectorySource(const std::filesystem::path& root) {
    std::error_code error;
    root_ = std::filesystem::canonical(root, error);
    if (error) {
        throw_path_error(root, "could not resolve asset root: " + error.message());
    }
    if (!std::filesystem::is_directory(root_, error) || error) {
        throw_path_error(root_, "asset root is not a directory");
    }
}

const std::filesystem::path& DirectorySource::root() const noexcept {
    return root_;
}

std::filesystem::path DirectorySource::resolve(const std::filesystem::path& asset_name) const {
    if (asset_name.empty()) {
        throw_path_error(asset_name, "asset name is empty");
    }
    if (asset_name.is_absolute() || asset_name.has_root_name() || asset_name.has_root_directory()) {
        throw_path_error(asset_name, "asset name must be relative to the asset root");
    }

    const auto normalized_name = asset_name.lexically_normal();
    for (const auto& part : normalized_name) {
        if (part == "..") {
            throw_path_error(asset_name, "asset name escapes the asset root");
        }
    }

    std::error_code error;
    const auto candidate = std::filesystem::weakly_canonical(root_ / normalized_name, error);
    if (error) {
        throw_path_error(asset_name, "could not resolve asset path: " + error.message());
    }
    if (!is_within(root_, candidate)) {
        throw_path_error(asset_name, "asset path escapes the asset root");
    }
    return candidate;
}

Bytes DirectorySource::read(const std::filesystem::path& asset_name) const {
    const auto path = resolve(asset_name);
    std::error_code error;
    const auto byte_count = std::filesystem::file_size(path, error);
    if (error) {
        throw_path_error(asset_name, "could not determine file size: " + error.message());
    }
    if (byte_count > std::numeric_limits<std::size_t>::max()) {
        throw_path_error(asset_name, "file is too large to load");
    }

    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw_path_error(asset_name, "could not open file");
    }

    Bytes bytes(static_cast<std::size_t>(byte_count));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            throw_path_error(asset_name, "could not read complete file");
        }
    }
    return bytes;
}

} // namespace mycore::assets

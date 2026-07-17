#pragma once

#include <filesystem>

namespace mycore::platform_sdl {

class Runtime {
public:
    Runtime();
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;
};

[[nodiscard]] std::filesystem::path application_base_path();

} // namespace mycore::platform_sdl

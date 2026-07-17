#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace mycore::platform_sdl {

class StartupError : public std::runtime_error {
public:
    explicit StartupError(std::string message);

    [[nodiscard]] static StartupError from_sdl(std::string_view operation);
};

} // namespace mycore::platform_sdl

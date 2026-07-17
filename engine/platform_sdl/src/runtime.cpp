#include "mycore/platform_sdl/runtime.hpp"

#include "mycore/platform_sdl/error.hpp"

#include <SDL3/SDL.h>
#include <string>
#include <utility>

namespace mycore::platform_sdl {

StartupError::StartupError(std::string message)
    : std::runtime_error(std::move(message)) {}

StartupError StartupError::from_sdl(std::string_view operation) {
    return StartupError{std::string{operation} + ": " + SDL_GetError()};
}

Runtime::Runtime() {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        throw StartupError::from_sdl("Could not initialize SDL video");
    }
}

Runtime::~Runtime() {
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

std::filesystem::path application_base_path() {
    const auto* path = SDL_GetBasePath();
    if (path == nullptr || *path == '\0') {
        throw StartupError::from_sdl("Could not locate the application base path");
    }
    return std::filesystem::path{path};
}

} // namespace mycore::platform_sdl

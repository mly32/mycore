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

} // namespace mycore::platform_sdl

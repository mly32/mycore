#pragma once

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

} // namespace mycore::platform_sdl

#pragma once

#include "dots/client/client_config.hpp"

namespace dots::client {

enum class ClientRunMode : std::uint8_t {
    Game,
    InMemoryGame,
    HeadlessSmoke,
    PackageSmoke,
};

int run_client(const ClientConfig& config, ClientRunMode mode = ClientRunMode::Game);

} // namespace dots::client

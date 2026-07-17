#pragma once

#include "dots/client/client_config.hpp"

namespace dots::client {

enum class ClientRunMode {
    Game,
    HeadlessSmoke,
    PackageSmoke,
};

int run_client(const ClientConfig& config, ClientRunMode mode = ClientRunMode::Game);

} // namespace dots::client

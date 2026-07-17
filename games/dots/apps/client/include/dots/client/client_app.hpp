#pragma once

#include "dots/client/client_config.hpp"

namespace dots::client {

int run_client(const ClientConfig& config, bool headless_smoke);

} // namespace dots::client

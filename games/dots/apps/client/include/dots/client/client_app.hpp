#pragma once

#include "dots/client/client_config.hpp"
#include "dots/protocol/messages.hpp"
#include "mycore/net_transport/net_transport.hpp"

namespace dots::client {

enum class ClientRunMode : std::uint8_t {
    Game,
    InMemoryGame,
    NativeGame,
    HeadlessSmoke,
    PackageSmoke,
};

struct ClientRunOptions {
    ClientRunMode mode{ClientRunMode::Game};
    dots::protocol::JoinRole join_role{dots::protocol::JoinRole::Player};
    std::string server_address{"127.0.0.1:27020"};
    mycore::net_transport::NetworkImpairment impairment;
};

int run_client(const ClientConfig& config, const ClientRunOptions& options = {});

} // namespace dots::client

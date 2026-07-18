#pragma once

#include "dots/simulation/world.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <cstddef>
#include <memory>
#include <optional>

namespace dots::server {

enum class RuntimeError : std::uint8_t {
    ClientIdExhausted,
    EntityIdExhausted,
    SnapshotIdExhausted,
    TickOutOfRange,
    InvalidWorldState,
    SimulationInputRejected,
    SimulationStepFailed,
    ProtocolEncodeFailed,
};

class Runtime {
public:
    explicit Runtime(mycore::net_transport::Endpoint& endpoint,
                     simulation::World initial_world = {});
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;

    [[nodiscard]] std::optional<RuntimeError> process_events();
    [[nodiscard]] std::optional<RuntimeError> step();

    [[nodiscard]] const simulation::World& world() const noexcept;
    [[nodiscard]] std::size_t client_count() const noexcept;
    [[nodiscard]] std::size_t rejected_packet_count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dots::server

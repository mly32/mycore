#pragma once

#include "dots/server/input_provenance.hpp"
#include "dots/simulation/world.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace dots::server {

enum class RuntimeError : std::uint8_t {
    ClientIdExhausted,
    PlayerOwnerIdExhausted,
    EntityIdExhausted,
    NoSafeSpawn,
    SnapshotIdExhausted,
    TickOutOfRange,
    InvalidWorldState,
    SimulationInputRejected,
    SimulationStepFailed,
    ProtocolEncodeFailed,
};

inline constexpr std::uint32_t kDefaultLivenessTimeoutTicks = 90;
inline constexpr std::uint32_t kDefaultHandshakeTimeoutTicks = 300;
inline constexpr std::uint32_t kDefaultInputHoldTicks = 5;
inline constexpr std::uint32_t kDefaultRespawnCooldownTicks = 90;

struct RuntimeSettings {
    std::uint32_t liveness_timeout_ticks{kDefaultLivenessTimeoutTicks};
    std::uint32_t handshake_timeout_ticks{kDefaultHandshakeTimeoutTicks};
    std::uint32_t input_hold_ticks{kDefaultInputHoldTicks};
    std::uint32_t respawn_cooldown_ticks{kDefaultRespawnCooldownTicks};
    std::size_t input_provenance_record_capacity{};
};

class Runtime {
public:
    explicit Runtime(mycore::net_transport::Endpoint& endpoint,
                     simulation::World initial_world = {},
                     RuntimeSettings settings = {});
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
    [[nodiscard]] std::vector<InputProvenanceRecord> take_input_provenance_records();
    [[nodiscard]] InputProvenanceSummary input_provenance_summary() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dots::server

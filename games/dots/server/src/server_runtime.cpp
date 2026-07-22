#include "dots/server/server_runtime.hpp"

#include "dots/protocol/codec.hpp"
#include "dots/replication/replication.hpp"
#include "dots/simulation/input_command.hpp"
#include "mycore/debug/log.hpp"
#include "mycore/math/vector2.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace dots::server {
namespace {

using mycore::net_transport::ConnectionHandle;
using mycore::net_transport::DeliveryMode;
using mycore::net_transport::SendStatus;

[[nodiscard]] constexpr std::string_view
disconnect_reason_name(mycore::net_transport::DisconnectReason reason) noexcept {
    using mycore::net_transport::DisconnectReason;
    switch (reason) {
    case DisconnectReason::LocalRequest:
        return "local request";
    case DisconnectReason::RemoteRequest:
        return "remote request";
    case DisconnectReason::TransportFailure:
        return "transport failure";
    }
    return "unknown reason";
}

struct Session {
    ConnectionHandle connection;
    protocol::ClientId client_id;
    simulation::EntityId player_id;
    protocol::InputSequenceId last_processed_input_id;
    std::map<std::uint32_t, protocol::InputSample> pending_inputs;
    std::uint32_t next_snapshot_id{};

    [[nodiscard]] bool ready() const noexcept {
        return client_id.is_valid() && player_id.is_valid();
    }
};

enum class InputEnqueueResult : std::uint8_t {
    Accepted,
    ConflictingDuplicate,
    Overflow,
};

inline constexpr std::size_t kSpawnColumns = 11;
inline constexpr std::size_t kSpawnRows = 7;
inline constexpr std::size_t kSpawnPointCount = kSpawnColumns * kSpawnRows;
inline constexpr std::size_t kSpawnStride = 37;
inline constexpr std::size_t kSpawnOffset = 23;

[[nodiscard]] mycore::math::Vector2 spawn_position(std::uint32_t ordinal) noexcept {
    const auto slot =
        (static_cast<std::size_t>(ordinal) * kSpawnStride + kSpawnOffset) % kSpawnPointCount;
    const auto column = slot % kSpawnColumns;
    const auto row = slot / kSpawnColumns;
    return {
        -76.0F + (static_cast<float>(column) * 16.0F),
        -44.0F + (static_cast<float>(row) * 16.0F),
    };
}

[[nodiscard]] bool spawn_is_clear(const simulation::World& world,
                                  mycore::math::Vector2 candidate) noexcept {
    constexpr auto kMinimumDistanceSquared = 64.0F;
    for (const auto player_id : world.player_ids()) {
        const auto current_position = world.position(player_id);
        if (current_position &&
            mycore::math::length_squared(*current_position - candidate) < kMinimumDistanceSquared) {
            return false;
        }
    }
    return true;
}

} // namespace

class Runtime::Impl {
public:
    Impl(mycore::net_transport::Endpoint& endpoint, simulation::World initial_world)
        : endpoint_(endpoint),
          world_(std::move(initial_world)) {}

    [[nodiscard]] std::optional<RuntimeError> process_events() {
        for (const auto& event : endpoint_.poll()) {
            if (const auto* connected = std::get_if<mycore::net_transport::Connected>(&event)) {
                const auto [unused, inserted] =
                    sessions_.try_emplace(connected->connection.value(),
                                          Session{
                                              .connection = connected->connection,
                                              .client_id = {},
                                              .player_id = {},
                                              .last_processed_input_id = {},
                                              .pending_inputs = {},
                                              .next_snapshot_id = 0,
                                          });
                static_cast<void>(unused);
                if (inserted) {
                    mycore::debug::log_info("dots.server.session",
                                            "Transport connection {} opened",
                                            connected->connection.value());
                }
                continue;
            }
            if (const auto* disconnected =
                    std::get_if<mycore::net_transport::Disconnected>(&event)) {
                log_disconnect(*disconnected);
                remove_session(disconnected->connection);
                continue;
            }

            const auto& received = std::get<mycore::net_transport::PayloadReceived>(event);
            const auto session_iterator = sessions_.find(received.connection.value());
            if (session_iterator == sessions_.end()) {
                continue;
            }
            const auto decoded = protocol::decode(received.payload);
            const auto* message = std::get_if<protocol::Message>(&decoded);
            if (message == nullptr) {
                reject(received.connection);
                continue;
            }

            if (std::holds_alternative<protocol::ClientHello>(*message)) {
                if (received.delivery != DeliveryMode::Reliable ||
                    session_iterator->second.ready()) {
                    reject(received.connection);
                    continue;
                }
                if (const auto error = accept(session_iterator->second)) {
                    return error;
                }
                continue;
            }

            const auto* input = std::get_if<protocol::InputPacket>(message);
            if (input == nullptr || received.delivery != DeliveryMode::Unreliable ||
                !session_iterator->second.ready()) {
                reject(received.connection);
                continue;
            }
            auto& session = session_iterator->second;
            const auto enqueue_result = enqueue_input(session, *input);
            if (enqueue_result == InputEnqueueResult::ConflictingDuplicate) {
                reject(received.connection, "conflicting duplicate input sample");
                continue;
            }
            if (enqueue_result == InputEnqueueResult::Overflow) {
                reject(received.connection, "pending input queue overflow");
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<RuntimeError> step() {
        std::vector<std::pair<std::uint32_t, protocol::InputSequenceId>> applied_inputs;
        applied_inputs.reserve(sessions_.size());
        for (auto& [connection_value, session] : sessions_) {
            if (!session.ready() || session.pending_inputs.empty()) {
                continue;
            }
            const auto& sample = session.pending_inputs.begin()->second;
            if (!world_.apply_input({
                    .id = replication::to_simulation(sample.sequence_id),
                    .entity_id = session.player_id,
                    .movement = {sample.movement_x, sample.movement_y},
                })) {
                return RuntimeError::SimulationInputRejected;
            }
            applied_inputs.emplace_back(connection_value, sample.sequence_id);
        }

        if (!world_.step()) {
            return RuntimeError::SimulationStepFailed;
        }
        for (const auto& [connection_value, sequence_id] : applied_inputs) {
            auto& session = sessions_.at(connection_value);
            session.last_processed_input_id = sequence_id;
            session.pending_inputs.erase(sequence_id.value());
        }
        if ((world_.tick().value() % 2U) != 0U) {
            return std::nullopt;
        }

        std::vector<ConnectionHandle> connections;
        connections.reserve(sessions_.size());
        for (const auto& [unused, session] : sessions_) {
            static_cast<void>(unused);
            if (session.ready()) {
                connections.push_back(session.connection);
            }
        }
        for (const auto connection : connections) {
            if (const auto error = send_snapshot(connection)) {
                return error;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] const simulation::World& world() const noexcept {
        return world_;
    }

    [[nodiscard]] std::size_t client_count() const noexcept {
        std::size_t count{};
        for (const auto& [unused, session] : sessions_) {
            static_cast<void>(unused);
            count += session.ready() ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] std::size_t rejected_packet_count() const noexcept {
        return rejected_packet_count_;
    }

private:
    [[nodiscard]] static InputEnqueueResult enqueue_input(Session& session,
                                                          const protocol::InputPacket& packet) {
        std::vector<protocol::InputSample> fresh_samples;
        fresh_samples.reserve(packet.samples.size());
        for (const auto& sample : packet.samples) {
            if (session.last_processed_input_id.is_valid() &&
                sample.sequence_id <= session.last_processed_input_id) {
                continue;
            }
            const auto existing = session.pending_inputs.find(sample.sequence_id.value());
            if (existing != session.pending_inputs.end()) {
                if (existing->second != sample) {
                    return InputEnqueueResult::ConflictingDuplicate;
                }
                continue;
            }
            fresh_samples.push_back(sample);
        }

        if (session.pending_inputs.size() + fresh_samples.size() >
            protocol::kMaximumPendingInputCount) {
            return InputEnqueueResult::Overflow;
        }
        for (const auto& sample : fresh_samples) {
            session.pending_inputs.emplace(sample.sequence_id.value(), sample);
        }
        return InputEnqueueResult::Accepted;
    }

    [[nodiscard]] std::optional<RuntimeError> accept(Session& session) {
        if (world_.tick().value() > std::numeric_limits<std::uint32_t>::max()) {
            return RuntimeError::TickOutOfRange;
        }
        if (next_client_id_ == protocol::ClientId::kInvalidValue) {
            return RuntimeError::ClientIdExhausted;
        }
        auto player = std::optional<simulation::EntityId>{};
        for (std::size_t attempt = 0; attempt < kSpawnPointCount; ++attempt) {
            const auto position = spawn_position(next_spawn_ordinal_++);
            if (!spawn_is_clear(world_, position)) {
                continue;
            }
            player = world_.spawn_player(position);
            if (player) {
                break;
            }
        }
        if (!player) {
            return RuntimeError::EntityIdExhausted;
        }

        session.client_id = protocol::ClientId{next_client_id_++};
        session.player_id = *player;
        const auto connection = session.connection;
        const protocol::ServerWelcome welcome{
            .client_id = session.client_id,
            .controlled_entity_id = replication::to_protocol(session.player_id),
            .server_tick = static_cast<std::uint32_t>(world_.tick().value()),
        };
        if (const auto error = transmit(connection, welcome, DeliveryMode::Reliable)) {
            return error;
        }
        if (sessions_.contains(connection.value())) {
            const auto snapshot_error = send_snapshot(connection);
            if (!snapshot_error && sessions_.contains(connection.value())) {
                if (const auto position = world_.position(session.player_id)) {
                    mycore::debug::log_info("dots.server.session",
                                            "Client {} joined on connection {} controlling entity "
                                            "{} at ({:.1f}, {:.1f})",
                                            session.client_id.value(),
                                            connection.value(),
                                            session.player_id.value(),
                                            position->x,
                                            position->y);
                }
            }
            return snapshot_error;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<RuntimeError> send_snapshot(ConnectionHandle connection) {
        const auto iterator = sessions_.find(connection.value());
        if (iterator == sessions_.end()) {
            return std::nullopt;
        }
        auto& session = iterator->second;
        if (session.next_snapshot_id == protocol::SnapshotId::kInvalidValue) {
            return RuntimeError::SnapshotIdExhausted;
        }
        const auto snapshot = replication::build_full_snapshot(
            world_,
            protocol::SnapshotId{session.next_snapshot_id},
            session.last_processed_input_id,
            static_cast<std::uint8_t>(session.pending_inputs.size()));
        if (const auto* error = std::get_if<replication::SnapshotBuildError>(&snapshot)) {
            switch (*error) {
            case replication::SnapshotBuildError::InvalidSnapshotId:
                return RuntimeError::SnapshotIdExhausted;
            case replication::SnapshotBuildError::TickOutOfRange:
                return RuntimeError::TickOutOfRange;
            case replication::SnapshotBuildError::InvalidWorldState:
                return RuntimeError::InvalidWorldState;
            }
        }
        const auto transmit_error = transmit(
            connection, std::get<protocol::FullSnapshot>(snapshot), DeliveryMode::Unreliable);
        if (!transmit_error && sessions_.contains(connection.value())) {
            ++session.next_snapshot_id;
        }
        return transmit_error;
    }

    [[nodiscard]] std::optional<RuntimeError>
    transmit(ConnectionHandle connection, const protocol::Message& message, DeliveryMode delivery) {
        const auto encoded = protocol::encode(message);
        const auto* bytes = std::get_if<protocol::EncodedMessage>(&encoded);
        if (bytes == nullptr) {
            return RuntimeError::ProtocolEncodeFailed;
        }
        if (endpoint_.send(connection, *bytes, delivery) != SendStatus::Sent) {
            mycore::debug::log_warning("dots.server.session",
                                       "Send failed on connection {}; removing its session",
                                       connection.value());
            remove_session(connection);
        }
        return std::nullopt;
    }

    void reject(ConnectionHandle connection, std::string_view reason = "invalid packet") {
        ++rejected_packet_count_;
        mycore::debug::log_warning("dots.server.session",
                                   "Rejected {} on connection {}; disconnecting peer",
                                   reason,
                                   connection.value());
        static_cast<void>(endpoint_.disconnect(connection));
        remove_session(connection);
    }

    void log_disconnect(const mycore::net_transport::Disconnected& disconnected) const {
        const auto iterator = sessions_.find(disconnected.connection.value());
        if (iterator != sessions_.end() && iterator->second.ready()) {
            mycore::debug::log_info("dots.server.session",
                                    "Client {} disconnected from connection {} ({})",
                                    iterator->second.client_id.value(),
                                    disconnected.connection.value(),
                                    disconnect_reason_name(disconnected.reason));
            return;
        }
        mycore::debug::log_info("dots.server.session",
                                "Transport connection {} closed before handshake ({})",
                                disconnected.connection.value(),
                                disconnect_reason_name(disconnected.reason));
    }

    void remove_session(ConnectionHandle connection) {
        const auto iterator = sessions_.find(connection.value());
        if (iterator == sessions_.end()) {
            return;
        }
        if (iterator->second.player_id.is_valid()) {
            static_cast<void>(world_.remove_player(iterator->second.player_id));
        }
        sessions_.erase(iterator);
    }

    mycore::net_transport::Endpoint& endpoint_;
    simulation::World world_;
    std::unordered_map<std::uint32_t, Session> sessions_;
    std::uint32_t next_spawn_ordinal_{};
    std::uint32_t next_client_id_{};
    std::size_t rejected_packet_count_{};
};

Runtime::Runtime(mycore::net_transport::Endpoint& endpoint, simulation::World initial_world)
    : impl_(std::make_unique<Impl>(endpoint, std::move(initial_world))) {}

Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

std::optional<RuntimeError> Runtime::process_events() {
    return impl_->process_events();
}

std::optional<RuntimeError> Runtime::step() {
    return impl_->step();
}

const simulation::World& Runtime::world() const noexcept {
    return impl_->world();
}

std::size_t Runtime::client_count() const noexcept {
    return impl_->client_count();
}

std::size_t Runtime::rejected_packet_count() const noexcept {
    return impl_->rejected_packet_count();
}

} // namespace dots::server

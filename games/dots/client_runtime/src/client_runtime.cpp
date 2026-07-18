#include "dots/client_runtime/client_runtime.hpp"

#include "dots/protocol/codec.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace dots::client_runtime {
namespace {

using mycore::net_transport::DeliveryMode;
using mycore::net_transport::SendStatus;

} // namespace

class Runtime::Impl {
public:
    explicit Impl(mycore::net_transport::Endpoint& endpoint)
        : endpoint_(endpoint) {}

    [[nodiscard]] std::optional<RuntimeError> process_events() {
        for (const auto& event : endpoint_.poll()) {
            if (const auto* connected = std::get_if<mycore::net_transport::Connected>(&event)) {
                if (connection_.is_valid()) {
                    return fail(RuntimeError::MultipleConnections);
                }
                connection_ = connected->connection;
                state_ = State::Handshaking;
                if (!transmit(protocol::ClientHello{}, DeliveryMode::Reliable)) {
                    return fail(RuntimeError::TransportSendFailed);
                }
                continue;
            }
            if (std::holds_alternative<mycore::net_transport::Disconnected>(event)) {
                state_ = State::Disconnected;
                continue;
            }

            const auto& received = std::get<mycore::net_transport::PayloadReceived>(event);
            const auto decoded = protocol::decode(received.payload);
            const auto* message = std::get_if<protocol::Message>(&decoded);
            if (message == nullptr) {
                return fail(RuntimeError::ProtocolDecodeFailed);
            }
            if (const auto* welcome = std::get_if<protocol::ServerWelcome>(message)) {
                if (received.delivery != DeliveryMode::Reliable || client_id_.is_valid()) {
                    return fail(RuntimeError::UnexpectedMessage);
                }
                client_id_ = welcome->client_id;
                controlled_entity_id_ = welcome->controlled_entity_id;
                if (const auto error = update_ready_state()) {
                    return error;
                }
                continue;
            }
            if (const auto* snapshot = std::get_if<protocol::FullSnapshot>(message)) {
                if (received.delivery != DeliveryMode::Unreliable) {
                    return fail(RuntimeError::UnexpectedMessage);
                }
                const auto result = world_.apply(*snapshot);
                if (result == replication::SnapshotApplyResult::Invalid) {
                    return fail(RuntimeError::InvalidSnapshot);
                }
                if (const auto error = update_ready_state()) {
                    return error;
                }
                continue;
            }
            return fail(RuntimeError::UnexpectedMessage);
        }
        return std::nullopt;
    }

    [[nodiscard]] InputSendResult send_input(std::uint32_t client_tick,
                                             mycore::math::Vector2 movement) {
        if (state_ != State::Ready) {
            return InputSendResult::NotReady;
        }
        if (next_input_id_ == protocol::InputSequenceId::kInvalidValue) {
            return InputSendResult::SequenceExhausted;
        }
        const protocol::InputCommand input{
            .sequence_id = protocol::InputSequenceId{next_input_id_},
            .client_tick = client_tick,
            .movement_x = movement.x,
            .movement_y = movement.y,
            .last_received_snapshot_id = world_.snapshot_id(),
        };
        const auto encoded = protocol::encode(input);
        const auto* bytes = std::get_if<protocol::EncodedMessage>(&encoded);
        if (bytes == nullptr) {
            return InputSendResult::InvalidMovement;
        }
        if (endpoint_.send(connection_, *bytes, DeliveryMode::Unreliable) != SendStatus::Sent) {
            state_ = State::Disconnected;
            return InputSendResult::TransportFailure;
        }
        ++next_input_id_;
        return InputSendResult::Sent;
    }

    [[nodiscard]] bool disconnect() {
        return connection_.is_valid() && endpoint_.disconnect(connection_);
    }

    [[nodiscard]] State state() const noexcept {
        return state_;
    }

    [[nodiscard]] const replication::ReplicatedWorld& world() const noexcept {
        return world_;
    }

    [[nodiscard]] protocol::ClientId client_id() const noexcept {
        return client_id_;
    }

    [[nodiscard]] protocol::EntityId controlled_entity_id() const noexcept {
        return controlled_entity_id_;
    }

private:
    [[nodiscard]] std::optional<RuntimeError> update_ready_state() {
        if (!client_id_.is_valid() || !world_.snapshot_id().is_valid()) {
            return std::nullopt;
        }
        const auto* controlled = world_.find(controlled_entity_id_);
        if (controlled == nullptr || controlled->kind != protocol::EntityKind::Player) {
            return fail(RuntimeError::MissingControlledEntity);
        }
        state_ = State::Ready;
        return std::nullopt;
    }

    [[nodiscard]] bool transmit(const protocol::Message& message, DeliveryMode delivery) {
        const auto encoded = protocol::encode(message);
        const auto* bytes = std::get_if<protocol::EncodedMessage>(&encoded);
        return bytes != nullptr &&
               endpoint_.send(connection_, *bytes, delivery) == SendStatus::Sent;
    }

    [[nodiscard]] std::optional<RuntimeError> fail(RuntimeError error) {
        state_ = State::Failed;
        if (connection_.is_valid()) {
            static_cast<void>(endpoint_.disconnect(connection_));
        }
        return error;
    }

    mycore::net_transport::Endpoint& endpoint_;
    mycore::net_transport::ConnectionHandle connection_;
    State state_{State::Connecting};
    protocol::ClientId client_id_;
    protocol::EntityId controlled_entity_id_;
    replication::ReplicatedWorld world_;
    std::uint32_t next_input_id_{};
};

Runtime::Runtime(mycore::net_transport::Endpoint& endpoint)
    : impl_(std::make_unique<Impl>(endpoint)) {}

Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

std::optional<RuntimeError> Runtime::process_events() {
    return impl_->process_events();
}

InputSendResult Runtime::send_input(std::uint32_t client_tick, mycore::math::Vector2 movement) {
    return impl_->send_input(client_tick, movement);
}

bool Runtime::disconnect() {
    return impl_->disconnect();
}

State Runtime::state() const noexcept {
    return impl_->state();
}

const replication::ReplicatedWorld& Runtime::world() const noexcept {
    return impl_->world();
}

protocol::ClientId Runtime::client_id() const noexcept {
    return impl_->client_id();
}

protocol::EntityId Runtime::controlled_entity_id() const noexcept {
    return impl_->controlled_entity_id();
}

} // namespace dots::client_runtime

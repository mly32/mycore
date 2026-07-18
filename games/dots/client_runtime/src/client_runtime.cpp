#include "dots/client_runtime/client_runtime.hpp"

#include "dots/protocol/codec.hpp"
#include "mycore/debug/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace dots::client_runtime {
namespace {

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

} // namespace

class Runtime::Impl {
public:
    Impl(mycore::net_transport::Endpoint& endpoint, Settings settings)
        : endpoint_(endpoint),
          settings_(settings) {}

    [[nodiscard]] std::optional<RuntimeError>
    process_events(std::chrono::steady_clock::time_point now) {
        for (const auto& event : endpoint_.poll()) {
            if (const auto* connected = std::get_if<mycore::net_transport::Connected>(&event)) {
                if (connection_.is_valid()) {
                    return fail(RuntimeError::MultipleConnections);
                }
                connection_ = connected->connection;
                state_ = State::Handshaking;
                mycore::debug::log_info("dots.client.session",
                                        "Transport connection {} opened; starting handshake",
                                        connection_.value());
                if (!transmit(protocol::ClientHello{}, DeliveryMode::Reliable)) {
                    return fail(RuntimeError::TransportSendFailed);
                }
                continue;
            }
            if (const auto* disconnected =
                    std::get_if<mycore::net_transport::Disconnected>(&event)) {
                mycore::debug::log_info("dots.client.session",
                                        "Transport connection {} closed ({})",
                                        disconnected->connection.value(),
                                        disconnect_reason_name(disconnected->reason));
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
                if (result == replication::SnapshotApplyResult::Applied) {
                    prune_sent_input_samples();
                    snapshot_times_.push_back(now);
                    latest_snapshot_time_ = now;
                    ++accepted_snapshot_count_;
                    prune_snapshot_times(now);
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
        const protocol::InputSample sample{
            .sequence_id = protocol::InputSequenceId{next_input_id_},
            .client_tick = client_tick,
            .movement_x = movement.x,
            .movement_y = movement.y,
        };
        protocol::InputPacket packet{
            .last_received_snapshot_id = world_.snapshot_id(),
            .samples = {},
        };
        packet.samples.reserve(protocol::kMaximumInputSamplesPerPacket);
        if (settings_.input_redundancy) {
            packet.samples.insert(
                packet.samples.end(), sent_input_samples_.begin(), sent_input_samples_.end());
        }
        packet.samples.push_back(sample);

        const auto encoded = protocol::encode(packet);
        const auto* bytes = std::get_if<protocol::EncodedMessage>(&encoded);
        if (bytes == nullptr) {
            return InputSendResult::InvalidMovement;
        }
        if (endpoint_.send(connection_, *bytes, DeliveryMode::Unreliable) != SendStatus::Sent) {
            state_ = State::Disconnected;
            return InputSendResult::TransportFailure;
        }
        if (settings_.input_redundancy) {
            constexpr auto kRetainedPriorSampleCount = protocol::kMaximumInputSamplesPerPacket - 1;
            sent_input_samples_.push_back(sample);
            while (sent_input_samples_.size() > kRetainedPriorSampleCount) {
                sent_input_samples_.pop_front();
            }
        }
        ++next_input_id_;
        return InputSendResult::Sent;
    }

    [[nodiscard]] bool disconnect() {
        if (!connection_.is_valid() || state_ == State::Disconnected ||
            !endpoint_.disconnect(connection_)) {
            return false;
        }
        state_ = State::Disconnected;
        mycore::debug::log_info("dots.client.session",
                                "Requested disconnect for client {} on connection {}",
                                client_id_.value(),
                                connection_.value());
        return true;
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

    [[nodiscard]] mycore::net_transport::ConnectionHandle connection_handle() const noexcept {
        return connection_;
    }

    [[nodiscard]] ReplicationStatistics
    replication_statistics(std::chrono::steady_clock::time_point now) const noexcept {
        ReplicationStatistics result;
        result.accepted_snapshot_count = accepted_snapshot_count_;
        if (latest_snapshot_time_) {
            result.latest_snapshot_age =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::max(
                    now - *latest_snapshot_time_, std::chrono::steady_clock::duration::zero()));
        }
        const auto window_start = now - std::chrono::seconds{1};
        const auto first =
            std::lower_bound(snapshot_times_.begin(), snapshot_times_.end(), window_start);
        result.accepted_snapshots_per_second =
            static_cast<float>(std::distance(first, snapshot_times_.end()));
        return result;
    }

private:
    void prune_sent_input_samples() {
        const auto acknowledged = world_.last_processed_input_id();
        if (!acknowledged.is_valid()) {
            return;
        }
        while (!sent_input_samples_.empty() &&
               sent_input_samples_.front().sequence_id <= acknowledged) {
            sent_input_samples_.pop_front();
        }
    }

    void prune_snapshot_times(std::chrono::steady_clock::time_point now) {
        const auto retention_start = now - std::chrono::seconds{2};
        while (!snapshot_times_.empty() && snapshot_times_.front() < retention_start) {
            snapshot_times_.pop_front();
        }
    }

    [[nodiscard]] std::optional<RuntimeError> update_ready_state() {
        if (!client_id_.is_valid() || !world_.snapshot_id().is_valid()) {
            return std::nullopt;
        }
        const auto* controlled = world_.find(controlled_entity_id_);
        if (controlled == nullptr || controlled->kind != protocol::EntityKind::Player) {
            return fail(RuntimeError::MissingControlledEntity);
        }
        if (state_ != State::Ready) {
            state_ = State::Ready;
            mycore::debug::log_info("dots.client.session",
                                    "Session ready as client {} controlling entity {}",
                                    client_id_.value(),
                                    controlled_entity_id_.value());
        }
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
    Settings settings_;
    mycore::net_transport::ConnectionHandle connection_;
    State state_{State::Connecting};
    protocol::ClientId client_id_;
    protocol::EntityId controlled_entity_id_;
    replication::ReplicatedWorld world_;
    std::uint32_t next_input_id_{};
    std::deque<protocol::InputSample> sent_input_samples_;
    std::deque<std::chrono::steady_clock::time_point> snapshot_times_;
    std::optional<std::chrono::steady_clock::time_point> latest_snapshot_time_;
    std::uint64_t accepted_snapshot_count_{};
};

Runtime::Runtime(mycore::net_transport::Endpoint& endpoint, Settings settings)
    : impl_(std::make_unique<Impl>(endpoint, settings)) {}

Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

std::optional<RuntimeError> Runtime::process_events(std::chrono::steady_clock::time_point now) {
    return impl_->process_events(now);
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

mycore::net_transport::ConnectionHandle Runtime::connection_handle() const noexcept {
    return impl_->connection_handle();
}

ReplicationStatistics
Runtime::replication_statistics(std::chrono::steady_clock::time_point now) const noexcept {
    return impl_->replication_statistics(now);
}

} // namespace dots::client_runtime

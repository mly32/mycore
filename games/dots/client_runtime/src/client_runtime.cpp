#include "dots/client_runtime/client_runtime.hpp"

#include "dots/protocol/codec.hpp"
#include "dots/simulation/movement.hpp"
#include "mycore/debug/log.hpp"
#include "mycore/debug/profile.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <numeric>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace dots::client_runtime {
namespace {

using mycore::net_transport::DeliveryMode;
using mycore::net_transport::SendStatus;

constexpr float kCorrectionTolerance = 0.0001F;
constexpr auto kReplayBudget = std::chrono::milliseconds{2};
constexpr auto kWarningInterval = std::chrono::seconds{5};
constexpr std::size_t kReplayDurationSampleCapacity = 120;

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

[[nodiscard]] mycore::math::Vector2
advance_prediction(mycore::math::Vector2 position, const protocol::InputSample& sample) noexcept {
    const auto movement =
        simulation::normalized_player_movement({sample.movement_x, sample.movement_y});
    return simulation::advance_player_position(position, movement);
}

struct PredictionEntry {
    protocol::InputSample sample;
    mycore::math::Vector2 resulting_position;
};

class PredictionHistory {
public:
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] bool full() const noexcept {
        return size_ == entries_.size();
    }

    [[nodiscard]] const PredictionEntry& at(std::size_t index) const noexcept {
        return entries_[(begin_ + index) % entries_.size()];
    }

    [[nodiscard]] PredictionEntry& at(std::size_t index) noexcept {
        return entries_[(begin_ + index) % entries_.size()];
    }

    [[nodiscard]] bool push_back(PredictionEntry entry) noexcept {
        if (full()) {
            return false;
        }
        entries_[(begin_ + size_) % entries_.size()] = entry;
        ++size_;
        return true;
    }

    void discard_through(protocol::InputSequenceId acknowledgement) noexcept {
        if (!acknowledgement.is_valid()) {
            return;
        }
        while (size_ > 0 && at(0).sample.sequence_id <= acknowledgement) {
            begin_ = (begin_ + 1) % entries_.size();
            --size_;
        }
    }

    void clear() noexcept {
        begin_ = 0;
        size_ = 0;
    }

private:
    std::array<PredictionEntry, kPredictionHistoryCapacity> entries_{};
    std::size_t begin_{};
    std::size_t size_{};
};

struct SnapshotProcessResult {
    std::optional<RuntimeError> error;
    bool applied{};
};

} // namespace

class Runtime::Impl {
public:
    Impl(mycore::net_transport::Endpoint& endpoint, Settings settings)
        : endpoint_(endpoint),
          settings_(settings) {}

    [[nodiscard]] ProcessEventsResult process_events(std::chrono::steady_clock::time_point now) {
        ProcessEventsResult result;
        for (const auto& event : endpoint_.poll()) {
            if (const auto* connected = std::get_if<mycore::net_transport::Connected>(&event)) {
                if (connection_.is_valid()) {
                    result.error = fail(RuntimeError::MultipleConnections);
                    return result;
                }
                connection_ = connected->connection;
                state_ = State::Handshaking;
                mycore::debug::log_info("dots.client.session",
                                        "Transport connection {} opened; starting handshake",
                                        connection_.value());
                if (!transmit(protocol::ClientHello{}, DeliveryMode::Reliable)) {
                    result.error = fail(RuntimeError::TransportSendFailed);
                    return result;
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
                result.error = fail(RuntimeError::ProtocolDecodeFailed);
                return result;
            }
            if (const auto* welcome = std::get_if<protocol::ServerWelcome>(message)) {
                if (received.delivery != DeliveryMode::Reliable || client_id_.is_valid()) {
                    result.error = fail(RuntimeError::UnexpectedMessage);
                    return result;
                }
                client_id_ = welcome->client_id;
                controlled_entity_id_ = welcome->controlled_entity_id;
                if (const auto error = update_ready_state()) {
                    result.error = error;
                    return result;
                }
                continue;
            }
            if (const auto* snapshot = std::get_if<protocol::FullSnapshot>(message)) {
                if (received.delivery != DeliveryMode::Unreliable) {
                    result.error = fail(RuntimeError::UnexpectedMessage);
                    return result;
                }
                const auto snapshot_result = process_snapshot(*snapshot, now);
                if (snapshot_result.error) {
                    result.error = fail(*snapshot_result.error);
                    return result;
                }
                if (snapshot_result.applied) {
                    snapshot_times_.push_back(now);
                    latest_snapshot_time_ = now;
                    ++accepted_snapshot_count_;
                    prune_snapshot_times(now);
                    result.accepted_snapshots.push_back(
                        {.snapshot = *snapshot, .arrival_time = now});
                }
                if (const auto error = update_ready_state()) {
                    result.error = error;
                    return result;
                }
                continue;
            }
            result.error = fail(RuntimeError::UnexpectedMessage);
            return result;
        }
        return result;
    }

    [[nodiscard]] InputSendResult send_input(std::uint32_t client_tick,
                                             mycore::math::Vector2 movement) {
        if (state_ != State::Ready || !predicted_position_) {
            return InputSendResult::NotReady;
        }
        if (next_input_id_ == protocol::InputSequenceId::kInvalidValue) {
            return InputSendResult::SequenceExhausted;
        }
        if (last_sent_client_tick_ &&
            (*last_sent_client_tick_ == std::numeric_limits<std::uint32_t>::max() ||
             client_tick != *last_sent_client_tick_ + 1U)) {
            return InputSendResult::InvalidClientTick;
        }

        const protocol::InputSample sample{
            .sequence_id = protocol::InputSequenceId{next_input_id_},
            .client_tick = client_tick,
            .movement_x = movement.x,
            .movement_y = movement.y,
        };
        auto packet = make_input_packet(sample);
        auto encoded = protocol::encode(packet);
        auto* bytes = std::get_if<protocol::EncodedMessage>(&encoded);
        if (bytes == nullptr) {
            return InputSendResult::InvalidMovement;
        }

        if (input_history_.full()) {
            if (!hard_resync(std::chrono::steady_clock::now())) {
                static_cast<void>(fail(RuntimeError::MissingControlledEntity));
                return InputSendResult::NotReady;
            }
            packet = make_input_packet(sample);
            encoded = protocol::encode(packet);
            bytes = std::get_if<protocol::EncodedMessage>(&encoded);
            if (bytes == nullptr) {
                return InputSendResult::InvalidMovement;
            }
        }

        if (!predicted_position_) {
            return InputSendResult::NotReady;
        }
        const auto current_prediction = *predicted_position_;

        if (pending_injected_input_drop_count_ > 0) {
            --pending_injected_input_drop_count_;
            ++injected_input_drop_count_;
        } else if (endpoint_.send(connection_, *bytes, DeliveryMode::Unreliable) !=
                   SendStatus::Sent) {
            state_ = State::Disconnected;
            return InputSendResult::TransportFailure;
        }

        const auto resulting_position = advance_prediction(current_prediction, sample);
        if (!input_history_.push_back({
                .sample = sample,
                .resulting_position = resulting_position,
            })) {
            static_cast<void>(fail(RuntimeError::MissingControlledEntity));
            return InputSendResult::NotReady;
        }
        predicted_position_ = resulting_position;
        history_high_water_mark_ = std::max(history_high_water_mark_, input_history_.size());
        last_sent_client_tick_ = client_tick;
        ++next_input_id_;
        report_history_health(std::chrono::steady_clock::now());
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

    [[nodiscard]] std::optional<mycore::math::Vector2> predicted_position() const noexcept {
        return predicted_position_;
    }

    [[nodiscard]] std::optional<mycore::math::Vector2> pre_correction_position() const noexcept {
        return pre_correction_position_;
    }

    [[nodiscard]] std::span<const mycore::math::Vector2> latest_replay_path() const noexcept {
        return {latest_replay_path_.data(), latest_replay_path_.size()};
    }

    [[nodiscard]] std::span<const mycore::math::Vector2>
    latest_correction_replay_path() const noexcept {
        return {latest_correction_replay_path_.data(), latest_correction_replay_path_.size()};
    }

    [[nodiscard]] bool debug_inject_prediction_error(mycore::math::Vector2 displacement) {
        if (state_ != State::Ready || !predicted_position_ || !std::isfinite(displacement.x) ||
            !std::isfinite(displacement.y) || displacement == mycore::math::Vector2{}) {
            return false;
        }
        const auto injected_position = *predicted_position_ + displacement;
        if (!std::isfinite(injected_position.x) || !std::isfinite(injected_position.y)) {
            return false;
        }
        *predicted_position_ = injected_position;
        ++injected_prediction_error_count_;
        mycore::debug::log_warning("dots.client.prediction",
                                   "Injected client-only prediction error ({:.3f}, {:.3f})",
                                   displacement.x,
                                   displacement.y);
        return true;
    }

    [[nodiscard]] bool debug_drop_next_input_packets(std::size_t count) {
        if (state_ != State::Ready || count == 0 ||
            count > kPredictionHistoryCapacity - pending_injected_input_drop_count_) {
            return false;
        }
        pending_injected_input_drop_count_ += count;
        mycore::debug::log_warning(
            "dots.client.prediction", "Armed {} client-only injected input packet drops", count);
        return true;
    }

    [[nodiscard]] PredictionStatistics
    prediction_statistics(std::chrono::steady_clock::time_point now) const noexcept {
        PredictionStatistics result{
            .input_redundancy_enabled = settings_.input_redundancy,
            .last_input_sent = {},
            .last_input_acknowledged = world_.last_processed_input_id(),
            .unacknowledged_input_count = 0,
            .history_count = input_history_.size(),
            .history_capacity = kPredictionHistoryCapacity,
            .history_high_water_mark = history_high_water_mark_,
            .latest_server_pending_input_count = world_.pending_input_count(),
            .server_pending_input_high_water_mark = server_pending_input_high_water_mark_,
            .rollback_snapshot_id = rollback_snapshot_id_,
            .rollback_server_tick = rollback_server_tick_,
            .rollback_input_acknowledgement = rollback_input_acknowledgement_,
            .latest_replay_count = latest_replay_count_,
            .total_replayed_input_count = total_replayed_input_count_,
            .maximum_replay_count = maximum_replay_count_,
            .latest_replay_milliseconds = latest_replay_milliseconds_,
            .average_replay_milliseconds = 0.0,
            .maximum_replay_milliseconds = maximum_replay_milliseconds_,
            .reconciliation_count = reconciliation_count_,
            .nonzero_correction_count = nonzero_correction_count_,
            .latest_correction_distance = latest_correction_distance_,
            .maximum_correction_distance = maximum_correction_distance_,
            .corrections_per_minute = 0.0F,
            .accumulated_correction_displacement = accumulated_correction_displacement_,
            .correction_sequence_since_hard_resync = correction_sequence_since_hard_resync_,
            .replay_over_budget_count = replay_over_budget_count_,
            .hard_resync_count = hard_resync_count_,
            .pending_injected_input_drop_count = pending_injected_input_drop_count_,
            .injected_input_drop_count = injected_input_drop_count_,
            .injected_prediction_error_count = injected_prediction_error_count_,
        };
        if (next_input_id_ > 0) {
            result.last_input_sent = protocol::InputSequenceId{next_input_id_ - 1U};
            const auto acknowledgement = result.last_input_acknowledged;
            result.unacknowledged_input_count = acknowledgement.is_valid()
                                                    ? next_input_id_ - acknowledgement.value() - 1U
                                                    : next_input_id_;
        }
        if (replay_duration_sample_count_ > 0) {
            const auto begin = replay_duration_samples_.begin();
            const auto end = begin + static_cast<std::ptrdiff_t>(replay_duration_sample_count_);
            result.average_replay_milliseconds = std::accumulate(begin, end, 0.0) /
                                                 static_cast<double>(replay_duration_sample_count_);
        }
        const auto window_start = now - std::chrono::minutes{1};
        const auto first =
            std::lower_bound(correction_times_.begin(), correction_times_.end(), window_start);
        result.corrections_per_minute =
            static_cast<float>(std::distance(first, correction_times_.end()));
        return result;
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
    [[nodiscard]] protocol::InputPacket
    make_input_packet(const protocol::InputSample& current_sample) const {
        protocol::InputPacket packet{
            .last_received_snapshot_id = world_.snapshot_id(),
            .samples = {},
        };
        packet.samples.reserve(protocol::kMaximumInputSamplesPerPacket);
        if (settings_.input_redundancy) {
            constexpr auto kPriorSampleCount = protocol::kMaximumInputSamplesPerPacket - 1;
            const auto first = input_history_.size() > kPriorSampleCount
                                   ? input_history_.size() - kPriorSampleCount
                                   : 0;
            for (auto index = first; index < input_history_.size(); ++index) {
                packet.samples.push_back(input_history_.at(index).sample);
            }
        }
        packet.samples.push_back(current_sample);
        return packet;
    }

    [[nodiscard]] SnapshotProcessResult
    process_snapshot(const protocol::FullSnapshot& snapshot,
                     std::chrono::steady_clock::time_point now) {
        auto candidate_world = world_;
        const auto apply_result = candidate_world.apply(snapshot);
        if (apply_result == replication::SnapshotApplyResult::Invalid) {
            return {.error = RuntimeError::InvalidSnapshot};
        }
        if (apply_result == replication::SnapshotApplyResult::Stale) {
            return {};
        }
        if (!valid_acknowledgement(candidate_world.last_processed_input_id())) {
            return {.error = RuntimeError::InvalidInputAcknowledgement};
        }

        const protocol::EntityState* controlled{};
        if (controlled_entity_id_.is_valid()) {
            controlled = candidate_world.find(controlled_entity_id_);
            if (controlled == nullptr || controlled->kind != protocol::EntityKind::Player) {
                return {.error = RuntimeError::MissingControlledEntity};
            }
        }

        if (state_ == State::Ready) {
            if (controlled == nullptr || !predicted_position_) {
                return {.error = RuntimeError::MissingControlledEntity};
            }
            const auto controlled_state = *controlled;
            const auto previous_prediction = *predicted_position_;
            return reconcile_snapshot(
                std::move(candidate_world), controlled_state, previous_prediction, now);
        }

        world_ = std::move(candidate_world);
        record_server_pending_input(world_.pending_input_count());
        return {.error = {}, .applied = true};
    }

    [[nodiscard]] bool
    valid_acknowledgement(protocol::InputSequenceId acknowledgement) const noexcept {
        const auto previous = world_.last_processed_input_id();
        if (!acknowledgement.is_valid()) {
            return !previous.is_valid();
        }
        if (next_input_id_ == 0 || acknowledgement.value() >= next_input_id_) {
            return false;
        }
        return !previous.is_valid() || acknowledgement >= previous;
    }

    [[nodiscard]] SnapshotProcessResult
    reconcile_snapshot(replication::ReplicatedWorld candidate_world,
                       const protocol::EntityState& controlled,
                       mycore::math::Vector2 previous_prediction,
                       std::chrono::steady_clock::time_point now) {
        MYCORE_PROFILE_ZONE("Dots prediction reconciliation");
        const auto replay_start = std::chrono::steady_clock::now();
        auto scratch_history = input_history_;
        scratch_history.discard_through(candidate_world.last_processed_input_id());

        mycore::math::Vector2 scratch_position{controlled.position_x, controlled.position_y};
        std::vector<mycore::math::Vector2> scratch_replay_path;
        scratch_replay_path.reserve(scratch_history.size());
        for (std::size_t index = 0; index < scratch_history.size(); ++index) {
            auto& entry = scratch_history.at(index);
            scratch_position = advance_prediction(scratch_position, entry.sample);
            entry.resulting_position = scratch_position;
            scratch_replay_path.push_back(scratch_position);
        }

        const auto correction_distance =
            mycore::math::length(previous_prediction - scratch_position);
        const auto nonzero_correction = correction_distance > kCorrectionTolerance;

        world_ = std::move(candidate_world);
        input_history_ = scratch_history;
        predicted_position_ = scratch_position;
        latest_replay_path_ = std::move(scratch_replay_path);
        if (nonzero_correction) {
            pre_correction_position_ = previous_prediction;
            latest_correction_replay_path_ = latest_replay_path_;
            accumulated_correction_displacement_ =
                accumulated_correction_displacement_ + (previous_prediction - scratch_position);
        }
        const auto replay_duration = std::chrono::steady_clock::now() - replay_start;

        rollback_snapshot_id_ = world_.snapshot_id();
        rollback_server_tick_ = world_.server_tick();
        rollback_input_acknowledgement_ = world_.last_processed_input_id();
        latest_replay_count_ = input_history_.size();
        total_replayed_input_count_ += latest_replay_count_;
        maximum_replay_count_ = std::max(maximum_replay_count_, latest_replay_count_);
        ++reconciliation_count_;
        latest_correction_distance_ = correction_distance;
        maximum_correction_distance_ = std::max(maximum_correction_distance_, correction_distance);
        prune_correction_times(now);
        if (nonzero_correction) {
            ++nonzero_correction_count_;
            ++correction_sequence_since_hard_resync_;
            correction_times_.push_back(now);
        }
        record_replay_duration(replay_duration, now);
        record_server_pending_input(world_.pending_input_count());
        report_history_health(now);
        return {.error = {}, .applied = true};
    }

    [[nodiscard]] bool hard_resync(std::chrono::steady_clock::time_point now) {
        const auto* controlled = world_.find(controlled_entity_id_);
        if (controlled == nullptr || controlled->kind != protocol::EntityKind::Player) {
            return false;
        }
        predicted_position_ = {controlled->position_x, controlled->position_y};
        input_history_.clear();
        pre_correction_position_.reset();
        latest_replay_path_.clear();
        latest_correction_replay_path_.clear();
        accumulated_correction_displacement_ = {};
        correction_sequence_since_hard_resync_ = 0;
        ++hard_resync_count_;
        mycore::debug::log_warning(
            "dots.client.prediction",
            "Prediction history reached its {}-input capacity; hard-resynced to snapshot {}",
            kPredictionHistoryCapacity,
            world_.snapshot_id().value());
        report_history_health(now);
        return true;
    }

    void record_replay_duration(std::chrono::steady_clock::duration duration,
                                std::chrono::steady_clock::time_point now) {
        const auto milliseconds = std::chrono::duration<double, std::milli>{duration}.count();
        latest_replay_milliseconds_ = milliseconds;
        maximum_replay_milliseconds_ = std::max(maximum_replay_milliseconds_, milliseconds);
        replay_duration_samples_[replay_duration_next_index_] = milliseconds;
        replay_duration_next_index_ =
            (replay_duration_next_index_ + 1) % replay_duration_samples_.size();
        replay_duration_sample_count_ =
            std::min(replay_duration_sample_count_ + 1, replay_duration_samples_.size());

        if (duration <= kReplayBudget) {
            return;
        }
        ++replay_over_budget_count_;
        if (!last_replay_warning_time_ || now - *last_replay_warning_time_ >= kWarningInterval) {
            mycore::debug::log_warning(
                "dots.client.prediction",
                "Prediction replay exceeded 2 ms: {:.3f} ms for {} inputs at snapshot {}",
                milliseconds,
                latest_replay_count_,
                rollback_snapshot_id_.value());
            last_replay_warning_time_ = now;
        }
    }

    void report_history_health(std::chrono::steady_clock::time_point now) {
        const auto above_warning_threshold =
            input_history_.size() * 4 > kPredictionHistoryCapacity * 3;
        if (above_warning_threshold) {
            if (!history_pressure_warning_active_ || !last_history_warning_time_ ||
                now - *last_history_warning_time_ >= kWarningInterval) {
                mycore::debug::log_warning("dots.client.prediction",
                                           "Prediction history pressure is {}/{} inputs ({:.1f}%)",
                                           input_history_.size(),
                                           kPredictionHistoryCapacity,
                                           (100.0 * static_cast<double>(input_history_.size())) /
                                               static_cast<double>(kPredictionHistoryCapacity));
                last_history_warning_time_ = now;
            }
            history_pressure_warning_active_ = true;
            return;
        }
        if (history_pressure_warning_active_) {
            mycore::debug::log_info("dots.client.prediction",
                                    "Prediction history pressure recovered to {}/{} inputs",
                                    input_history_.size(),
                                    kPredictionHistoryCapacity);
            history_pressure_warning_active_ = false;
        }
    }

    void record_server_pending_input(std::uint8_t pending_input_count) noexcept {
        server_pending_input_high_water_mark_ =
            std::max(server_pending_input_high_water_mark_, pending_input_count);
    }

    void prune_correction_times(std::chrono::steady_clock::time_point now) {
        const auto retention_start = now - std::chrono::minutes{1};
        while (!correction_times_.empty() && correction_times_.front() < retention_start) {
            correction_times_.pop_front();
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
            predicted_position_ = {controlled->position_x, controlled->position_y};
            input_history_.clear();
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
    std::optional<std::uint32_t> last_sent_client_tick_;
    PredictionHistory input_history_;
    std::optional<mycore::math::Vector2> predicted_position_;
    std::optional<mycore::math::Vector2> pre_correction_position_;
    std::vector<mycore::math::Vector2> latest_replay_path_;
    std::vector<mycore::math::Vector2> latest_correction_replay_path_;
    std::size_t history_high_water_mark_{};
    std::uint8_t server_pending_input_high_water_mark_{};
    protocol::SnapshotId rollback_snapshot_id_;
    std::uint32_t rollback_server_tick_{};
    protocol::InputSequenceId rollback_input_acknowledgement_;
    std::size_t latest_replay_count_{};
    std::uint64_t total_replayed_input_count_{};
    std::size_t maximum_replay_count_{};
    double latest_replay_milliseconds_{};
    double maximum_replay_milliseconds_{};
    std::array<double, kReplayDurationSampleCapacity> replay_duration_samples_{};
    std::size_t replay_duration_next_index_{};
    std::size_t replay_duration_sample_count_{};
    std::uint64_t reconciliation_count_{};
    std::uint64_t nonzero_correction_count_{};
    float latest_correction_distance_{};
    float maximum_correction_distance_{};
    mycore::math::Vector2 accumulated_correction_displacement_;
    std::uint64_t correction_sequence_since_hard_resync_{};
    std::deque<std::chrono::steady_clock::time_point> correction_times_;
    std::uint64_t replay_over_budget_count_{};
    std::uint64_t hard_resync_count_{};
    std::size_t pending_injected_input_drop_count_{};
    std::uint64_t injected_input_drop_count_{};
    std::uint64_t injected_prediction_error_count_{};
    std::optional<std::chrono::steady_clock::time_point> last_replay_warning_time_;
    std::optional<std::chrono::steady_clock::time_point> last_history_warning_time_;
    bool history_pressure_warning_active_{};
    std::deque<std::chrono::steady_clock::time_point> snapshot_times_;
    std::optional<std::chrono::steady_clock::time_point> latest_snapshot_time_;
    std::uint64_t accepted_snapshot_count_{};
};

Runtime::Runtime(mycore::net_transport::Endpoint& endpoint, Settings settings)
    : impl_(std::make_unique<Impl>(endpoint, settings)) {}

Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

ProcessEventsResult Runtime::process_events(std::chrono::steady_clock::time_point now) {
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

std::optional<mycore::math::Vector2> Runtime::predicted_position() const noexcept {
    return impl_->predicted_position();
}

std::optional<mycore::math::Vector2> Runtime::pre_correction_position() const noexcept {
    return impl_->pre_correction_position();
}

std::span<const mycore::math::Vector2> Runtime::latest_replay_path() const noexcept {
    return impl_->latest_replay_path();
}

std::span<const mycore::math::Vector2> Runtime::latest_correction_replay_path() const noexcept {
    return impl_->latest_correction_replay_path();
}

bool Runtime::debug_inject_prediction_error(mycore::math::Vector2 displacement) {
    return impl_->debug_inject_prediction_error(displacement);
}

bool Runtime::debug_drop_next_input_packets(std::size_t count) {
    return impl_->debug_drop_next_input_packets(count);
}

PredictionStatistics
Runtime::prediction_statistics(std::chrono::steady_clock::time_point now) const noexcept {
    return impl_->prediction_statistics(now);
}

ReplicationStatistics
Runtime::replication_statistics(std::chrono::steady_clock::time_point now) const noexcept {
    return impl_->replication_statistics(now);
}

} // namespace dots::client_runtime

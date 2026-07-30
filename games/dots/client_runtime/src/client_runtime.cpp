#include "dots/client_runtime/client_runtime.hpp"

#include "dots/prediction/model.hpp"
#include "dots/protocol/codec.hpp"
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
#include <map>
#include <numeric>
#include <optional>
#include <string>
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
constexpr std::size_t kRecentPredictionCorrectionCapacity = 256;
constexpr auto kInitialScopeEpoch = mycore::rollback::ScopeEpoch{0};
constexpr auto kPredictionScopeHorizonFloor = mycore::time::TickDelta{5};

template <class Container>
void append_observable_event_batch(Container& destination, prediction::Commit&& commit) {
    auto batch = mycore::rollback::event_batch_from_commit(std::move(commit));
    if (!batch.changes.empty() || !batch.retired_keys.empty() ||
        !batch.externally_retired_keys.empty()) {
        destination.push_back(std::move(batch));
    }
}

template <class Container>
void append_observable_event_batch(Container& destination, PredictionEventBatch&& batch) {
    if (!batch.changes.empty() || !batch.retired_keys.empty() ||
        !batch.externally_retired_keys.empty()) {
        destination.push_back(std::move(batch));
    }
}

[[nodiscard]] mycore::time::TickDelta replay_horizon(std::size_t input_count) noexcept {
    return mycore::time::TickDelta{
        std::max(kPredictionScopeHorizonFloor.value(), static_cast<std::uint64_t>(input_count))};
}

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

[[nodiscard]] constexpr std::string_view
respawn_result_name(protocol::RespawnResult result) noexcept {
    using protocol::RespawnResult;
    switch (result) {
    case RespawnResult::None:
        return "none";
    case RespawnResult::Accepted:
        return "accepted";
    case RespawnResult::RejectedCooldown:
        return "rejected: cooldown";
    case RespawnResult::RejectedNotSpectating:
        return "rejected: not spectating";
    case RespawnResult::RejectedNoSafeSpawn:
        return "rejected: no safe spawn";
    }
    return "unknown";
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

enum class TimelineAdvanceStatus : std::uint8_t {
    Deferred,
    StimulusUnavailable,
};

using TimelineAdvanceResult =
    std::variant<prediction::Commit, prediction::TimelineFailure, TimelineAdvanceStatus>;

enum class AuthorityInstallReason : std::uint8_t {
    NewAuthority,
    ScopeRebase,
    HardResync,
};

struct PredictedProjection {
    std::vector<protocol::EntityId> owned_entity_ids;
    protocol::EntityId primary_entity_id;
    std::optional<mycore::math::Vector2> position;
};

struct RemotePlayerDisplacement {
    simulation::EntityId entity_id;
    mycore::math::Vector2 previous;
    mycore::math::Vector2 current;
    float distance{};
    float mass{};
};

[[nodiscard]] constexpr std::string_view
prediction_error_name(prediction::PredictionErrorCode error) noexcept {
    using prediction::PredictionErrorCode;
    switch (error) {
    case PredictionErrorCode::InvalidScope:
        return "invalid scope";
    case PredictionErrorCode::IncompatibleRules:
        return "incompatible rules";
    case PredictionErrorCode::CheckpointOutsideScope:
        return "checkpoint outside scope";
    case PredictionErrorCode::CheckpointRestoreFailed:
        return "checkpoint restore failed";
    case PredictionErrorCode::InvalidStimulus:
        return "invalid stimulus";
    case PredictionErrorCode::TickFailed:
        return "tick failed";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
scope_build_error_name(prediction::ScopeBuildError error) noexcept {
    using prediction::ScopeBuildError;
    switch (error) {
    case ScopeBuildError::InvalidRequest:
        return "invalid request";
    case ScopeBuildError::InvalidCheckpoint:
        return "invalid checkpoint";
    case ScopeBuildError::MissingOwnedOwner:
        return "missing owned owner";
    case ScopeBuildError::IncompleteOwnedState:
        return "incomplete owned state";
    case ScopeBuildError::UnsupportedMechanic:
        return "unsupported mechanic";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
checkpoint_error_name(simulation::CheckpointRestoreError error) noexcept {
    using simulation::CheckpointRestoreError;
    switch (error) {
    case CheckpointRestoreError::InvalidRules:
        return "invalid rules";
    case CheckpointRestoreError::InvalidOrdering:
        return "invalid ordering";
    case CheckpointRestoreError::InvalidOwnerState:
        return "invalid owner state";
    case CheckpointRestoreError::InvalidEntityState:
        return "invalid entity state";
    case CheckpointRestoreError::InvalidGeometry:
        return "invalid geometry";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view tick_error_name(simulation::TickError error) noexcept {
    using simulation::TickError;
    switch (error) {
    case TickError::InvalidCommand:
        return "invalid command";
    case TickError::DuplicateOwnerCommand:
        return "duplicate owner command";
    case TickError::SimulationRejected:
        return "simulation rejected";
    }
    return "unknown";
}

void log_timeline_failure(std::string_view operation, const prediction::TimelineFailure& failure) {
    if (!failure.model_error) {
        mycore::debug::log_error("dots.client.prediction",
                                 "{} failed with rollback error {}",
                                 operation,
                                 mycore::rollback::timeline_error_name(failure.code));
        return;
    }

    const auto& model_error = *failure.model_error;
    const auto checkpoint_error = model_error.checkpoint_error
                                      ? checkpoint_error_name(*model_error.checkpoint_error)
                                      : std::string_view{"none"};
    const auto tick_error = model_error.tick_error ? tick_error_name(*model_error.tick_error)
                                                   : std::string_view{"none"};
    mycore::debug::log_error("dots.client.prediction",
                             "{} failed with rollback error {}: model {}, checkpoint {}, tick {}",
                             operation,
                             mycore::rollback::timeline_error_name(failure.code),
                             prediction_error_name(model_error.code),
                             checkpoint_error,
                             tick_error);
}

[[nodiscard]] std::string sequence_name(protocol::InputSequenceId sequence) {
    return sequence.is_valid() ? std::to_string(sequence.value()) : "none";
}

[[nodiscard]] std::string sequence_name(std::optional<mycore::rollback::CommandSequence> sequence) {
    return sequence ? std::to_string(sequence->value()) : "none";
}

[[nodiscard]] std::vector<RemotePlayerDisplacement>
remote_player_displacements(const simulation::WorldCheckpoint& previous,
                            const simulation::WorldCheckpoint& current,
                            simulation::PlayerOwnerId owned_owner_id) {
    std::vector<RemotePlayerDisplacement> result;
    auto previous_index = std::size_t{};
    auto current_index = std::size_t{};
    while (previous_index < previous.players.size() && current_index < current.players.size()) {
        const auto& old_player = previous.players[previous_index];
        const auto& new_player = current.players[current_index];
        if (old_player.entity_id < new_player.entity_id) {
            ++previous_index;
            continue;
        }
        if (new_player.entity_id < old_player.entity_id) {
            ++current_index;
            continue;
        }
        if (new_player.owner_id != owned_owner_id) {
            const auto distance = mycore::math::length(old_player.position - new_player.position);
            if (distance > kCorrectionTolerance) {
                result.push_back(RemotePlayerDisplacement{
                    .entity_id = new_player.entity_id,
                    .previous = old_player.position,
                    .current = new_player.position,
                    .distance = distance,
                    .mass = old_player.mass,
                });
            }
        }
        ++previous_index;
        ++current_index;
    }
    return result;
}

[[nodiscard]] std::optional<mycore::rollback::CommandSequence>
command_sequence(protocol::InputSequenceId input_id) noexcept {
    if (!input_id.is_valid()) {
        return std::nullopt;
    }
    return mycore::rollback::CommandSequence{input_id.value()};
}

[[nodiscard]] protocol::PredictionKey to_protocol(const simulation::PredictionKey& key) noexcept {
    return {
        .owner_id = protocol::PlayerOwnerId{key.owner_id.value()},
        .input_id = protocol::InputSequenceId{key.input_id.value()},
        .child_ordinal = key.child_ordinal,
    };
}

[[nodiscard]] bool contains(std::span<const simulation::PlayerOwnerId> values,
                            simulation::PlayerOwnerId value) {
    return std::binary_search(values.begin(), values.end(), value);
}

[[nodiscard]] bool scope_covers(const prediction::PredictionScope& current,
                                const prediction::PredictionScope& required) {
    const auto contains_all = [](const auto& superset, const auto& subset) {
        return std::ranges::includes(superset, subset);
    };
    // The required scope is rebuilt for the next retained-input depth before every advance.
    // An older certified horizon remains safe while its selected causal membership still
    // contains that freshly computed closure; only a membership or policy change needs a rebase.
    return current.requested_profile == required.requested_profile &&
           current.active_profile == required.active_profile &&
           current.fallback_reason == required.fallback_reason &&
           current.requested_mechanics == required.requested_mechanics &&
           current.mechanics == required.mechanics &&
           current.required_domains == required.required_domains &&
           (current.required_causal_channels & required.required_causal_channels) ==
               required.required_causal_channels &&
           current.owned_owner_ids == required.owned_owner_ids &&
           contains_all(current.subscribed_event_owner_ids, required.subscribed_event_owner_ids) &&
           contains_all(current.owner_ids, required.owner_ids) && current.rules == required.rules &&
           contains_all(current.player_ids, required.player_ids) &&
           contains_all(current.food_ids, required.food_ids);
}

[[nodiscard]] std::vector<protocol::EntityId>
scope_entity_ids(const prediction::PredictionScope& scope) {
    std::vector<protocol::EntityId> result;
    result.reserve(scope.player_ids.size() + scope.food_ids.size());
    for (const auto entity_id : scope.player_ids) {
        result.push_back(protocol::EntityId{entity_id.value()});
    }
    for (const auto entity_id : scope.food_ids) {
        result.push_back(protocol::EntityId{entity_id.value()});
    }
    std::ranges::sort(result);
    return result;
}

[[nodiscard]] PredictedProjection
project_predicted_owner(const simulation::World& world,
                        simulation::PlayerOwnerId owner_id,
                        protocol::EntityId confirmed_primary_entity_id,
                        std::optional<mycore::math::Vector2> fallback_position = std::nullopt) {
    PredictedProjection result{
        .owned_entity_ids = {},
        .primary_entity_id = {},
        .position = fallback_position,
    };
    for (const auto entity_id : world.player_ids()) {
        if (world.player_owner(entity_id) != owner_id) {
            continue;
        }
        const auto protocol_id = protocol::EntityId{entity_id.value()};
        result.owned_entity_ids.push_back(protocol_id);
        if (protocol_id == confirmed_primary_entity_id) {
            result.primary_entity_id = protocol_id;
        }
    }
    if (!result.primary_entity_id.is_valid() && !result.owned_entity_ids.empty()) {
        result.primary_entity_id = result.owned_entity_ids.front();
    }
    if (result.primary_entity_id.is_valid()) {
        result.position = world.position(simulation::EntityId{result.primary_entity_id.value()});
    }
    return result;
}

[[nodiscard]] std::variant<std::vector<PredictionIdentityRemap>, RuntimeError>
prediction_identity_remaps(const std::optional<simulation::WorldCheckpoint>& previous,
                           const simulation::WorldCheckpoint& current,
                           simulation::PlayerOwnerId owner_id) {
    std::vector<PredictionIdentityRemap> result;
    if (!previous) {
        return result;
    }
    std::map<simulation::PredictionKey, simulation::EntityId> current_by_key;
    for (const auto& player : current.players) {
        if (!player.prediction_key || player.owner_id != owner_id) {
            continue;
        }
        if (!current_by_key.emplace(*player.prediction_key, player.entity_id).second) {
            return RuntimeError::AmbiguousPredictionIdentity;
        }
    }
    for (const auto& player : previous->players) {
        if (!player.prediction_key || player.owner_id != owner_id) {
            continue;
        }
        const auto match = current_by_key.find(*player.prediction_key);
        if (match == current_by_key.end() || match->second == player.entity_id) {
            continue;
        }
        result.push_back({
            .prediction_key = to_protocol(*player.prediction_key),
            .previous_entity_id = protocol::EntityId{player.entity_id.value()},
            .current_entity_id = protocol::EntityId{match->second.value()},
        });
    }
    return result;
}

} // namespace

std::string_view runtime_error_name(RuntimeError error) noexcept {
    switch (error) {
    case RuntimeError::MultipleConnections:
        return "MULTIPLE CONNECTIONS";
    case RuntimeError::ProtocolEncodeFailed:
        return "PROTOCOL ENCODE FAILED";
    case RuntimeError::ProtocolDecodeFailed:
        return "PROTOCOL DECODE FAILED";
    case RuntimeError::UnexpectedMessage:
        return "UNEXPECTED MESSAGE";
    case RuntimeError::InvalidSnapshot:
        return "INVALID SNAPSHOT";
    case RuntimeError::InvalidInputAcknowledgement:
        return "INVALID INPUT ACKNOWLEDGEMENT";
    case RuntimeError::MissingControlledEntity:
        return "MISSING CONTROLLED ENTITY";
    case RuntimeError::CheckpointHydrationFailed:
        return "CHECKPOINT HYDRATION FAILED";
    case RuntimeError::PredictionScopeFailed:
        return "PREDICTION SCOPE FAILED";
    case RuntimeError::PredictionTimelineFailed:
        return "PREDICTION TIMELINE FAILED";
    case RuntimeError::PredictionEventQueueFull:
        return "PREDICTION EVENT QUEUE FULL";
    case RuntimeError::AmbiguousPredictionIdentity:
        return "AMBIGUOUS PREDICTION IDENTITY";
    case RuntimeError::TransportSendFailed:
        return "TRANSPORT SEND FAILED";
    }
    return "UNKNOWN";
}

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
                respawn_cooldown_ticks_ = welcome->respawn_cooldown_ticks;
                world_rules_ = welcome->world_rules;
                if (latest_authority_snapshot_) {
                    const auto prediction_result = install_authority(
                        *latest_authority_snapshot_, world_, authority_receipts_, now);
                    if (prediction_result) {
                        result.error = fail(*prediction_result);
                        return result;
                    }
                }
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
                                             mycore::math::Vector2 movement,
                                             std::uint16_t action_bits) {
        if (state_ != State::Ready) {
            return InputSendResult::NotReady;
        }
        if ((action_bits & static_cast<std::uint16_t>(~protocol::kKnownInputActionBits)) != 0U) {
            return InputSendResult::InvalidAction;
        }
        const auto playing = world_.recipient().mode == protocol::SessionMode::Playing;
        if (playing && (!timeline_ || !confirmed_owner_id_.is_valid())) {
            return InputSendResult::NotReady;
        }
        if (playing && prediction_event_batches_.size() >= kPredictionEventBatchCapacity) {
            static_cast<void>(fail(RuntimeError::PredictionEventQueueFull));
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
            .action_bits = action_bits,
        };
        auto packet = make_input_packet(sample);
        auto encoded = protocol::encode(packet);
        auto* bytes = std::get_if<protocol::EncodedMessage>(&encoded);
        if (bytes == nullptr) {
            return InputSendResult::InvalidMovement;
        }

        if (playing && input_history_.full()) {
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
        if (playing) {
            const auto scope_error = ensure_prediction_scope(input_history_.size() + 1U,
                                                             std::chrono::steady_clock::now());
            if (scope_error) {
                static_cast<void>(fail(*scope_error));
                return InputSendResult::NotReady;
            }
            if (prediction_event_batches_.size() >= kPredictionEventBatchCapacity) {
                static_cast<void>(fail(RuntimeError::PredictionEventQueueFull));
                return InputSendResult::NotReady;
            }
        }

        if (pending_injected_input_drop_count_ > 0) {
            --pending_injected_input_drop_count_;
            ++injected_input_drop_count_;
        } else if (endpoint_.send(connection_, *bytes, DeliveryMode::Unreliable) !=
                   SendStatus::Sent) {
            static_cast<void>(endpoint_.disconnect(connection_));
            state_ = State::Disconnected;
            return InputSendResult::TransportFailure;
        }

        last_sent_client_tick_ = client_tick;
        ++next_input_id_;
        if (!playing) {
            return InputSendResult::Sent;
        }
        if (!input_history_.push_back({
                .sample = sample,
                .resulting_position = predicted_position_.value_or(mycore::math::Vector2{}),
            })) {
            static_cast<void>(fail(RuntimeError::MissingControlledEntity));
            return InputSendResult::NotReady;
        }
        if (const auto error = advance_prediction(sample)) {
            static_cast<void>(fail(*error));
            return InputSendResult::NotReady;
        }
        history_high_water_mark_ = std::max(history_high_water_mark_, input_history_.size());
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

    [[nodiscard]] std::vector<PredictionEventBatch> take_prediction_event_batches() {
        std::vector<PredictionEventBatch> result;
        result.reserve(prediction_event_batches_.size());
        while (!prediction_event_batches_.empty()) {
            result.push_back(std::move(prediction_event_batches_.front()));
            prediction_event_batches_.pop_front();
        }
        return result;
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
        return primary_entity_id();
    }

    [[nodiscard]] protocol::SessionMode session_mode() const noexcept {
        return world_.recipient().mode;
    }

    [[nodiscard]] std::span<const protocol::EntityId> owned_entity_ids() const noexcept {
        return world_.recipient().owned_entity_ids;
    }

    [[nodiscard]] protocol::EntityId primary_entity_id() const noexcept {
        return world_.recipient().primary_entity_id;
    }

    [[nodiscard]] protocol::EntityId follow_entity_id() const noexcept {
        return world_.recipient().follow_entity_id;
    }

    [[nodiscard]] std::optional<protocol::WorldRules> world_rules() const noexcept {
        return world_rules_;
    }

    [[nodiscard]] std::optional<std::uint32_t> defeat_tick() const noexcept {
        return world_.recipient().defeat_tick;
    }

    [[nodiscard]] std::optional<std::uint32_t> respawn_available_tick() const noexcept {
        return world_.recipient().respawn_available_tick;
    }

    [[nodiscard]] std::uint32_t respawn_cooldown_ticks() const noexcept {
        return respawn_cooldown_ticks_;
    }

    [[nodiscard]] std::optional<protocol::PlayerAbsorbed> latest_absorption() const noexcept {
        return world_.recipient().latest_absorption;
    }

    [[nodiscard]] protocol::InputSequenceId latest_respawn_request_id() const noexcept {
        return world_.recipient().latest_respawn_request_id;
    }

    [[nodiscard]] protocol::RespawnResult latest_respawn_result() const noexcept {
        return world_.recipient().latest_respawn_result;
    }

    [[nodiscard]] mycore::net_transport::ConnectionHandle connection_handle() const noexcept {
        return connection_;
    }

    [[nodiscard]] const simulation::World* predicted_world() const noexcept {
        return timeline_ ? timeline_->state() : nullptr;
    }

    [[nodiscard]] protocol::EntityId predicted_primary_entity_id() const noexcept {
        return predicted_primary_entity_id_;
    }

    [[nodiscard]] std::span<const protocol::EntityId> predicted_owned_entity_ids() const noexcept {
        return predicted_owned_entity_ids_;
    }

    [[nodiscard]] std::span<const protocol::EntityId> predicted_scope_entity_ids() const noexcept {
        return predicted_scope_entity_ids_;
    }

    [[nodiscard]] std::span<const PredictionIdentityRemap>
    latest_prediction_identity_remaps() const noexcept {
        return latest_prediction_identity_remaps_;
    }

    [[nodiscard]] std::span<const PredictionCorrection>
    recent_prediction_corrections() const noexcept {
        return recent_prediction_corrections_;
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
        prediction_debug_offset_ = prediction_debug_offset_ + displacement;
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
            .last_timeline_input_submitted = timeline_submission_frontier(),
            .unacknowledged_input_count = 0,
            .deferred_prediction_input_count = deferred_prediction_input_count(),
            .history_count = input_history_.size(),
            .history_capacity = kPredictionHistoryCapacity,
            .history_high_water_mark = history_high_water_mark_,
            .scope_epoch = 0,
            .scope_replay_horizon_ticks = 0,
            .scope_owner_count = 0,
            .scope_event_owner_count = 0,
            .scope_player_count = 0,
            .scope_food_count = 0,
            .scope_rebase_count = scope_rebase_count_,
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
            .remote_entity_correction_count = remote_entity_correction_count_,
            .latest_remote_entity_correction_count = latest_remote_entity_correction_count_,
            .latest_correction_distance = latest_correction_distance_,
            .maximum_correction_distance = maximum_correction_distance_,
            .latest_remote_correction_distance = latest_remote_correction_distance_,
            .maximum_remote_correction_distance = maximum_remote_correction_distance_,
            .corrections_per_minute = 0.0F,
            .accumulated_correction_displacement = accumulated_correction_displacement_,
            .correction_sequence_since_hard_resync = correction_sequence_since_hard_resync_,
            .replay_over_budget_count = replay_over_budget_count_,
            .hard_resync_count = hard_resync_count_,
            .acknowledgement_catch_up_count = acknowledgement_catch_up_count_,
            .pending_injected_input_drop_count = pending_injected_input_drop_count_,
            .injected_input_drop_count = injected_input_drop_count_,
            .injected_prediction_error_count = injected_prediction_error_count_,
            .authority_receipts_accepted_through = authority_receipts_.accepted_through(),
            .authority_receipts_published_through = authority_receipts_.published_through(),
            .authority_receipts_server_retired_through =
                authority_receipts_.server_retired_through(),
            .authority_receipt_retained_count = authority_receipts_.retained_count(),
            .authority_receipt_pending_publication_count =
                authority_receipts_.pending_publication_count(),
            .pending_prediction_event_batch_count = prediction_event_batches_.size(),
        };
        if (timeline_ && timeline_->scope()) {
            const auto& scope = *timeline_->scope();
            result.scope_epoch = scope.scope_epoch.value();
            result.scope_replay_horizon_ticks = scope.replay_horizon.value();
            result.scope_owner_count = scope.owner_ids.size();
            result.scope_event_owner_count = scope.subscribed_event_owner_ids.size();
            result.scope_player_count = scope.player_ids.size();
            result.scope_food_count = scope.food_ids.size();
        }
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
    [[nodiscard]] protocol::InputSequenceId timeline_submission_frontier() const noexcept {
        if (!timeline_) {
            return {};
        }
        const auto submitted = timeline_->last_submitted_sequence();
        if (!submitted || submitted->value() >= protocol::InputSequenceId::kInvalidValue) {
            return {};
        }
        return protocol::InputSequenceId{static_cast<std::uint32_t>(submitted->value())};
    }

    [[nodiscard]] std::size_t deferred_prediction_input_count() const noexcept {
        const auto submitted = timeline_submission_frontier();
        auto count = std::size_t{};
        for (auto index = std::size_t{}; index < input_history_.size(); ++index) {
            if (!submitted.is_valid() || input_history_.at(index).sample.sequence_id > submitted) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] bool input_history_contains(protocol::InputSequenceId sequence) const noexcept {
        for (auto index = std::size_t{}; index < input_history_.size(); ++index) {
            if (input_history_.at(index).sample.sequence_id == sequence) {
                return true;
            }
        }
        return false;
    }

    void log_authority_frontiers(std::string_view context,
                                 const replication::ReplicatedWorld& candidate_world) const {
        const auto first = input_history_.size() > 0 ? input_history_.at(0).sample.sequence_id
                                                     : protocol::InputSequenceId{};
        const auto last = input_history_.size() > 0
                              ? input_history_.at(input_history_.size() - 1).sample.sequence_id
                              : protocol::InputSequenceId{};
        const auto last_sent = next_input_id_ > 0 ? protocol::InputSequenceId{next_input_id_ - 1U}
                                                  : protocol::InputSequenceId{};
        mycore::debug::log_error(
            "dots.client.prediction.frontier",
            "{} frontiers: authority ACK {}, timeline ACK {}, timeline submitted {}, "
            "retained inputs {} [{}..{}], deferred {}, last sent {}",
            context,
            sequence_name(candidate_world.last_processed_input_id()),
            sequence_name(timeline_ ? timeline_->acknowledged_through() : std::nullopt),
            sequence_name(timeline_ ? timeline_->last_submitted_sequence() : std::nullopt),
            input_history_.size(),
            sequence_name(first),
            sequence_name(last),
            deferred_prediction_input_count(),
            sequence_name(last_sent));
    }

    [[nodiscard]] protocol::InputPacket
    make_input_packet(const protocol::InputSample& current_sample) const {
        protocol::InputPacket packet{
            .last_received_snapshot_id = world_.snapshot_id(),
            .last_received_authority_receipt_sequence = authority_receipts_.published_through(),
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
        auto candidate_receipts = authority_receipts_;
        const auto apply_result = candidate_world.apply(snapshot);
        if (apply_result == replication::SnapshotApplyResult::Invalid) {
            return {.error = RuntimeError::InvalidSnapshot};
        }
        if (apply_result == replication::SnapshotApplyResult::Stale) {
            return {};
        }
        auto receipt_apply_result = candidate_receipts.apply(snapshot);
        const auto* receipt_delta =
            std::get_if<replication::AuthorityReceiptDelta>(&receipt_apply_result);
        if (receipt_delta == nullptr) {
            return {.error = RuntimeError::InvalidSnapshot};
        }
        if (!valid_acknowledgement(candidate_world.last_processed_input_id())) {
            log_authority_frontiers("Rejected session acknowledgement", candidate_world);
            return {.error = RuntimeError::InvalidInputAcknowledgement};
        }
        if (client_id_.is_valid() && !valid_respawn_deadline(candidate_world.recipient())) {
            return {.error = RuntimeError::InvalidSnapshot};
        }
        const auto previous_recipient = world_.recipient();

        if (world_rules_) {
            if (const auto error = install_authority(snapshot,
                                                     candidate_world,
                                                     candidate_receipts,
                                                     now,
                                                     0,
                                                     AuthorityInstallReason::NewAuthority,
                                                     receipt_delta)) {
                return {.error = error};
            }
        }
        world_ = std::move(candidate_world);
        authority_receipts_ = std::move(candidate_receipts);
        latest_authority_snapshot_ = snapshot;
        if (world_.recipient().mode == protocol::SessionMode::Spectating) {
            clear_prediction_state();
        }
        record_server_pending_input(world_.pending_input_count());
        log_recipient_changes(previous_recipient, snapshot.snapshot_id);
        return {.error = {}, .applied = true};
    }

    void log_recipient_changes(const protocol::RecipientSessionState& previous,
                               protocol::SnapshotId snapshot_id) const {
        if (!client_id_.is_valid()) {
            return;
        }
        const auto& current = world_.recipient();
        if (current.latest_absorption != previous.latest_absorption && current.latest_absorption) {
            const auto& event = *current.latest_absorption;
            mycore::debug::log_info(
                "dots.client.session",
                "Snapshot {} confirmed absorption at tick {}: entity {} (owner {}) absorbed "
                "entity {} (owner {}), transferring mass {:.3f}",
                snapshot_id.value(),
                event.server_tick,
                event.absorber_entity_id.value(),
                event.absorber_owner_id.value(),
                event.victim_entity_id.value(),
                event.victim_owner_id.value(),
                event.transferred_mass);
        }
        if (current.mode != previous.mode) {
            if (current.mode == protocol::SessionMode::Spectating && current.defeat_tick &&
                current.respawn_available_tick) {
                if (current.follow_entity_id.is_valid()) {
                    mycore::debug::log_info(
                        "dots.client.session",
                        "Snapshot {} confirmed client {} SPECTATING at defeat tick {}; follow "
                        "entity {}, respawn available tick {}",
                        snapshot_id.value(),
                        client_id_.value(),
                        *current.defeat_tick,
                        current.follow_entity_id.value(),
                        *current.respawn_available_tick);
                } else {
                    mycore::debug::log_info(
                        "dots.client.session",
                        "Snapshot {} confirmed client {} SPECTATING at defeat tick {}; no follow "
                        "entity, respawn available tick {}",
                        snapshot_id.value(),
                        client_id_.value(),
                        *current.defeat_tick,
                        *current.respawn_available_tick);
                }
            } else if (current.mode == protocol::SessionMode::Playing) {
                mycore::debug::log_info(
                    "dots.client.session",
                    "Snapshot {} confirmed client {} PLAYING with primary entity {}",
                    snapshot_id.value(),
                    client_id_.value(),
                    current.primary_entity_id.value());
            }
        } else if (previous.follow_entity_id.is_valid() && !current.follow_entity_id.is_valid()) {
            mycore::debug::log_info("dots.client.session",
                                    "Snapshot {} confirmed follow entity {} is no longer present",
                                    snapshot_id.value(),
                                    previous.follow_entity_id.value());
        }
        if (current.latest_respawn_request_id.is_valid() &&
            current.latest_respawn_request_id != previous.latest_respawn_request_id) {
            const auto result = respawn_result_name(current.latest_respawn_result);
            mycore::debug::log_info("dots.client.session",
                                    "Snapshot {} confirmed respawn input {}: {}",
                                    snapshot_id.value(),
                                    current.latest_respawn_request_id.value(),
                                    result);
        }
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

    [[nodiscard]] bool
    valid_respawn_deadline(const protocol::RecipientSessionState& session) const noexcept {
        if (session.mode != protocol::SessionMode::Spectating) {
            return true;
        }
        if (!session.defeat_tick || !session.respawn_available_tick ||
            respawn_cooldown_ticks_ >
                std::numeric_limits<std::uint32_t>::max() - *session.defeat_tick) {
            return false;
        }
        return *session.respawn_available_tick == *session.defeat_tick + respawn_cooldown_ticks_;
    }

    [[nodiscard]] std::optional<simulation::PlayerOwnerId>
    confirmed_owner(const replication::ReplicatedWorld& candidate_world) const noexcept {
        const auto primary_entity_id = candidate_world.recipient().primary_entity_id;
        const auto* controlled = candidate_world.find(primary_entity_id);
        if (controlled == nullptr || controlled->kind != protocol::EntityKind::Player ||
            !controlled->owner_id.is_valid()) {
            return std::nullopt;
        }
        return simulation::PlayerOwnerId{controlled->owner_id.value()};
    }

    [[nodiscard]] static std::optional<std::vector<prediction::HeldMovementAssumption>>
    remote_movement_assumptions(const simulation::WorldCheckpoint& checkpoint,
                                std::span<const simulation::PlayerOwnerId> owned_owner_ids,
                                const simulation::WorldCheckpoint& movement_authority) {
        std::vector<prediction::HeldMovementAssumption> result;
        result.reserve(checkpoint.owners.size());
        for (const auto& owner : checkpoint.owners) {
            if (contains(owned_owner_ids, owner.owner_id)) {
                continue;
            }
            const auto authoritative_owner = std::lower_bound(
                movement_authority.owners.begin(),
                movement_authority.owners.end(),
                owner.owner_id,
                [](const simulation::OwnerCheckpoint& candidate, simulation::PlayerOwnerId id) {
                    return candidate.owner_id < id;
                });
            if (authoritative_owner == movement_authority.owners.end() ||
                authoritative_owner->owner_id != owner.owner_id) {
                return std::nullopt;
            }
            result.push_back({
                .owner_id = owner.owner_id,
                .source_tick = movement_authority.tick,
                .movement = authoritative_owner->movement,
            });
        }
        return result;
    }

    [[nodiscard]] std::optional<prediction::TickStimulus>
    make_stimulus(const prediction::Timeline& timeline,
                  const protocol::InputSample& sample,
                  simulation::PlayerOwnerId owner_id,
                  const simulation::WorldCheckpoint& movement_authority) const {
        prediction::TickStimulus result;
        result.commands.push_back({
            .type = simulation::TickCommandType::ApplyInput,
            .input_id = simulation::InputCommandId{sample.sequence_id.value()},
            .owner_id = owner_id,
            .movement = {sample.movement_x, sample.movement_y},
            .split_requested = (sample.action_bits & protocol::kSplitActionBit) != 0U,
        });
        auto assumptions = remote_movement_assumptions(
            timeline.state()->checkpoint(), timeline.scope()->owned_owner_ids, movement_authority);
        if (!assumptions) {
            return std::nullopt;
        }
        result.remote_movement_assumptions = std::move(*assumptions);
        return result;
    }

    [[nodiscard]] static std::variant<prediction::TickStimulus, prediction::PredictionError>
    refresh_remote_assumptions(const prediction::TickStimulus& previous,
                               const simulation::World& replay_state,
                               const prediction::PredictionScope& scope,
                               const simulation::WorldCheckpoint& movement_authority) {
        auto refreshed = previous;
        auto assumptions = remote_movement_assumptions(
            replay_state.checkpoint(), scope.owned_owner_ids, movement_authority);
        if (!assumptions) {
            return prediction::PredictionError{
                .code = prediction::PredictionErrorCode::InvalidStimulus,
                .checkpoint_error = std::nullopt,
                .tick_error = std::nullopt,
            };
        }
        refreshed.remote_movement_assumptions = std::move(*assumptions);
        return refreshed;
    }

    [[nodiscard]] TimelineAdvanceResult
    advance_timeline(prediction::Timeline& timeline,
                     const protocol::InputSample& sample,
                     simulation::PlayerOwnerId owner_id,
                     const simulation::WorldCheckpoint& movement_authority) const {
        const auto checkpoint = timeline.state()->checkpoint();
        const auto live_owner = std::lower_bound(
            checkpoint.owners.begin(),
            checkpoint.owners.end(),
            owner_id,
            [](const simulation::OwnerCheckpoint& owner, simulation::PlayerOwnerId value) {
                return owner.owner_id < value;
            });
        if (live_owner == checkpoint.owners.end() || live_owner->owner_id != owner_id) {
            return TimelineAdvanceStatus::Deferred;
        }
        const auto stimulus = make_stimulus(timeline, sample, owner_id, movement_authority);
        if (!stimulus) {
            return TimelineAdvanceStatus::StimulusUnavailable;
        }
        auto advanced = timeline.advance(
            mycore::rollback::CommandSequence{sample.sequence_id.value()}, *stimulus);
        if (auto* commit = std::get_if<prediction::Commit>(&advanced)) {
            return std::move(*commit);
        }
        return std::get<prediction::TimelineFailure>(std::move(advanced));
    }

    [[nodiscard]] std::vector<prediction::AuthorityEvent>
    authority_events(std::span<const protocol::AuthorityReceipt> receipts) const {
        std::vector<prediction::AuthorityEvent> result;
        result.reserve(receipts.size());
        for (const auto& receipt : receipts) {
            auto event = replication::to_simulation(receipt.event);
            result.push_back({
                .disposition = mycore::rollback::AuthorityEventDisposition::Confirmed,
                .key = simulation::simulation_event_key(event),
                .event = event,
            });
        }
        return result;
    }

    [[nodiscard]] std::variant<prediction::PredictionScope, RuntimeError>
    required_scope(const simulation::WorldCheckpoint& authority,
                   simulation::PlayerOwnerId owner_id,
                   mycore::rollback::ScopeEpoch scope_epoch,
                   mycore::time::TickDelta required_replay_horizon) const {
        auto result = prediction::build_prediction_scope(
            authority,
            {
                .profile = prediction::PredictionProfile::InteractionClosure,
                .mechanics = prediction::kCurrentPredictionMechanics,
                .owned_owner_ids = {owner_id},
                .subscribed_event_owner_ids = {owner_id},
                .replay_horizon = required_replay_horizon,
                .scope_epoch = scope_epoch,
                .coverage = {},
            });
        if (auto* scope = std::get_if<prediction::PredictionScope>(&result)) {
            return std::move(*scope);
        }
        mycore::debug::log_error(
            "dots.client.prediction",
            "Prediction scope build failed at authority tick {} for owner {}, horizon {}: {}",
            authority.tick.value(),
            owner_id.value(),
            required_replay_horizon.value(),
            scope_build_error_name(std::get<prediction::ScopeBuildError>(result)));
        return RuntimeError::PredictionScopeFailed;
    }

    [[nodiscard]] static mycore::time::Tick
    simulation_event_tick(const simulation::SimulationEvent& event) {
        return std::visit(
            [](const auto& value) {
                return value.tick;
            },
            event);
    }

    static void remember_replay_retirements(
        std::map<simulation::SimulationEventKey, mycore::time::Tick>& pending_retirements,
        std::span<const PredictionEventBatch> batches) {
        for (const auto& batch : batches) {
            for (const auto& key : batch.retired_keys) {
                const auto change = std::find_if(
                    batch.changes.begin(), batch.changes.end(), [&key](const auto& candidate) {
                        return candidate.key == key;
                    });
                if (change == batch.changes.end()) {
                    continue;
                }
                const auto* event = change->current
                                        ? &*change->current
                                        : (change->previous ? &*change->previous : nullptr);
                if (event != nullptr) {
                    pending_retirements.insert_or_assign(key, simulation_event_tick(*event));
                }
            }
        }
    }

    static void append_unique_event_key(std::vector<simulation::SimulationEventKey>& destination,
                                        const simulation::SimulationEventKey& key) {
        if (std::find(destination.begin(), destination.end(), key) == destination.end()) {
            destination.push_back(key);
        }
    }

    static void attach_external_retirement_evidence(
        const replication::AuthorityReceiptInbox& candidate_receipts,
        const replication::AuthorityReceiptDelta* receipt_delta,
        std::map<simulation::SimulationEventKey, mycore::time::Tick>& pending_retirements,
        std::vector<PredictionEventBatch>& batches) {
        remember_replay_retirements(pending_retirements, batches);

        std::vector<simulation::SimulationEventKey> externally_retired;
        if (receipt_delta != nullptr) {
            externally_retired.reserve(receipt_delta->externally_retired_keys.size());
            for (const auto& key : receipt_delta->externally_retired_keys) {
                append_unique_event_key(externally_retired, key);
            }
        }
        if (receipt_delta != nullptr && receipt_delta->complete_coverage_through_server_tick) {
            const auto coverage_tick =
                mycore::time::Tick{*receipt_delta->complete_coverage_through_server_tick};
            for (const auto& [key, occurrence_tick] : pending_retirements) {
                if (occurrence_tick <= coverage_tick &&
                    !candidate_receipts.contains_event_key(key)) {
                    append_unique_event_key(externally_retired, key);
                }
            }
        }
        if (externally_retired.empty()) {
            return;
        }
        for (const auto& key : externally_retired) {
            pending_retirements.erase(key);
        }
        if (batches.empty()) {
            batches.push_back({
                .kind = mycore::rollback::CommitKind::AuthorityOnly,
                .changes = {},
                .retired_keys = {},
                .externally_retired_keys = std::move(externally_retired),
            });
            return;
        }
        auto& destination = batches.back().externally_retired_keys;
        for (const auto& key : externally_retired) {
            append_unique_event_key(destination, key);
        }
    }

    [[nodiscard]] std::optional<RuntimeError>
    install_authority(const protocol::FullSnapshot& snapshot,
                      const replication::ReplicatedWorld& candidate_world,
                      replication::AuthorityReceiptInbox& candidate_receipts,
                      std::chrono::steady_clock::time_point now,
                      std::size_t minimum_replay_input_count = 0,
                      AuthorityInstallReason reason = AuthorityInstallReason::NewAuthority,
                      const replication::AuthorityReceiptDelta* receipt_delta = nullptr) {
        MYCORE_PROFILE_ZONE("Dots prediction reconciliation");
        const auto replay_start = std::chrono::steady_clock::now();
        if (!world_rules_) {
            return std::nullopt;
        }
        const auto hydrated = replication::hydrate_checkpoint(snapshot, *world_rules_);
        const auto* authority = std::get_if<simulation::WorldCheckpoint>(&hydrated);
        if (authority == nullptr) {
            return RuntimeError::CheckpointHydrationFailed;
        }
        const auto pending_receipts = candidate_receipts.pending_publication();
        auto pending_authority_events = authority_events(pending_receipts);
        if (candidate_world.recipient().mode == protocol::SessionMode::Spectating) {
            std::vector<PredictionEventBatch> committed_batches;
            if (timeline_ && timeline_->scope()) {
                const auto projected =
                    prediction::project_checkpoint(*authority, *timeline_->scope());
                const auto* projected_authority =
                    std::get_if<simulation::WorldCheckpoint>(&projected);
                if (projected_authority == nullptr) {
                    const auto& error = std::get<prediction::PredictionError>(projected);
                    mycore::debug::log_error(
                        "dots.client.prediction",
                        "Terminal authority projection failed at tick {} in scope epoch {}: {}",
                        authority->tick.value(),
                        timeline_->scope_epoch().value(),
                        prediction_error_name(error.code));
                    return RuntimeError::PredictionScopeFailed;
                }
                prediction::Timeline scratch_timeline = *timeline_;
                const prediction::AuthorityFrame frame{
                    .tick = projected_authority->tick,
                    .acknowledged_through =
                        command_sequence(candidate_world.last_processed_input_id()),
                    .scope_epoch = timeline_->scope_epoch(),
                    .checkpoint = *projected_authority,
                    .events = std::move(pending_authority_events),
                };
                auto resynced = scratch_timeline.hard_resync(frame, *timeline_->scope());
                auto* commit = std::get_if<prediction::Commit>(&resynced);
                if (commit == nullptr) {
                    const auto& failure = std::get<prediction::TimelineFailure>(resynced);
                    log_timeline_failure("Terminal hard resync", failure);
                    return RuntimeError::PredictionTimelineFailed;
                }
                append_observable_event_batch(committed_batches, std::move(*commit));
            } else {
                auto resolved =
                    mycore::rollback::resolve_authority_only_events<prediction::WorldModel>(
                        prediction::WorldModel{}, pending_authority_events);
                auto* batch = std::get_if<PredictionEventBatch>(&resolved);
                if (batch == nullptr) {
                    mycore::debug::log_error(
                        "dots.client.prediction",
                        "Terminal authority-only event resolution failed with rollback error {}",
                        mycore::rollback::timeline_error_name(
                            std::get<mycore::rollback::TimelineErrorCode>(resolved)));
                    return RuntimeError::PredictionTimelineFailed;
                }
                append_observable_event_batch(committed_batches, std::move(*batch));
            }
            auto scratch_retirements = pending_consequence_retirements_;
            attach_external_retirement_evidence(
                candidate_receipts, receipt_delta, scratch_retirements, committed_batches);
            if (prediction_event_batches_.size() + committed_batches.size() >
                kPredictionEventBatchCapacity) {
                return RuntimeError::PredictionEventQueueFull;
            }
            if (!pending_receipts.empty() &&
                !candidate_receipts.mark_published_through(pending_receipts.back().sequence_id)) {
                mycore::debug::log_error(
                    "dots.client.prediction",
                    "Terminal authority receipt publication failed at sequence {} (accepted {}, "
                    "published {})",
                    pending_receipts.back().sequence_id.value(),
                    candidate_receipts.accepted_through().value(),
                    candidate_receipts.published_through().value());
                return RuntimeError::PredictionTimelineFailed;
            }
            for (auto& batch : committed_batches) {
                prediction_event_batches_.push_back(std::move(batch));
            }
            pending_consequence_retirements_ = std::move(scratch_retirements);
            return std::nullopt;
        }
        const auto owner = confirmed_owner(candidate_world);
        if (!owner) {
            return RuntimeError::MissingControlledEntity;
        }

        const auto previous_deferred_input_count = deferred_prediction_input_count();
        const auto previous_timeline_submission =
            timeline_ ? timeline_->last_submitted_sequence() : std::nullopt;
        auto scratch_history = input_history_;
        scratch_history.discard_through(candidate_world.last_processed_input_id());
        const auto required_replay_horizon =
            replay_horizon(std::max(scratch_history.size(), minimum_replay_input_count));
        const auto initial_epoch = timeline_ ? timeline_->scope_epoch() : kInitialScopeEpoch;
        auto required_result =
            required_scope(*authority, *owner, initial_epoch, required_replay_horizon);
        auto* required = std::get_if<prediction::PredictionScope>(&required_result);
        if (required == nullptr) {
            return std::get<RuntimeError>(required_result);
        }

        auto selected_scope = *required;
        auto rebuild = !timeline_ || !timeline_->scope() || confirmed_owner_id_ != *owner ||
                       !scope_covers(*timeline_->scope(), selected_scope);
        std::optional<simulation::WorldCheckpoint> projected_authority;
        if (!rebuild) {
            auto retained_projection =
                prediction::project_checkpoint(*authority, *timeline_->scope());
            if (auto* checkpoint = std::get_if<simulation::WorldCheckpoint>(&retained_projection)) {
                projected_authority = std::move(*checkpoint);
            } else {
                rebuild = true;
            }
        }
        if (!rebuild) {
            selected_scope = *timeline_->scope();
        } else if (timeline_) {
            if (timeline_->scope_epoch().value() ==
                mycore::rollback::ScopeEpoch::kInvalidValue - 1U) {
                return RuntimeError::PredictionScopeFailed;
            }
            const auto next_epoch =
                mycore::rollback::ScopeEpoch{timeline_->scope_epoch().value() + 1U};
            required_result =
                required_scope(*authority, *owner, next_epoch, required_replay_horizon);
            required = std::get_if<prediction::PredictionScope>(&required_result);
            if (required == nullptr) {
                return std::get<RuntimeError>(required_result);
            }
            selected_scope = *required;
        }

        if (!projected_authority) {
            auto projected_result = prediction::project_checkpoint(*authority, selected_scope);
            if (auto* checkpoint = std::get_if<simulation::WorldCheckpoint>(&projected_result)) {
                projected_authority = std::move(*checkpoint);
            } else {
                const auto& error = std::get<prediction::PredictionError>(projected_result);
                mycore::debug::log_error(
                    "dots.client.prediction",
                    "Authority projection failed at tick {} in scope epoch {}: {} (owners {}, "
                    "players {}, food {})",
                    authority->tick.value(),
                    selected_scope.scope_epoch.value(),
                    prediction_error_name(error.code),
                    selected_scope.owner_ids.size(),
                    selected_scope.player_ids.size(),
                    selected_scope.food_ids.size());
                return RuntimeError::PredictionScopeFailed;
            }
        }

        const auto previous_checkpoint =
            timeline_ && timeline_->state()
                ? std::optional<simulation::WorldCheckpoint>{timeline_->state()->checkpoint()}
                : std::nullopt;
        const auto previous_scope =
            timeline_ && timeline_->scope()
                ? std::optional<prediction::PredictionScope>{*timeline_->scope()}
                : std::nullopt;
        const auto previous_prediction = predicted_position_;
        const prediction::AuthorityFrame frame{
            .tick = projected_authority->tick,
            .acknowledged_through = command_sequence(candidate_world.last_processed_input_id()),
            .scope_epoch = selected_scope.scope_epoch,
            .checkpoint = *projected_authority,
            .events = std::move(pending_authority_events),
        };
        const auto acknowledgement_catch_up =
            reason != AuthorityInstallReason::HardResync && timeline_ &&
            confirmed_owner_id_ == *owner && frame.acknowledged_through &&
            (!previous_timeline_submission ||
             *frame.acknowledged_through > *previous_timeline_submission);
        if (acknowledgement_catch_up &&
            !input_history_contains(candidate_world.last_processed_input_id())) {
            mycore::debug::log_error(
                "dots.client.prediction.frontier",
                "Authority ACK {} is ahead of timeline submission {} but is not retained in the "
                "outer input history",
                sequence_name(candidate_world.last_processed_input_id()),
                sequence_name(previous_timeline_submission));
            log_authority_frontiers("Unrecoverable authority ACK gap", candidate_world);
            return RuntimeError::PredictionTimelineFailed;
        }

        prediction::Timeline scratch_timeline{
            prediction::WorldModel{},
            {.capacity = kPredictionHistoryCapacity},
        };
        std::vector<PredictionEventBatch> committed_batches;
        if ((reason == AuthorityInstallReason::HardResync || acknowledgement_catch_up) &&
            timeline_ && confirmed_owner_id_ == *owner) {
            scratch_timeline = *timeline_;
            auto resynced = scratch_timeline.hard_resync(frame, selected_scope);
            auto* commit = std::get_if<prediction::Commit>(&resynced);
            if (commit == nullptr) {
                log_timeline_failure(acknowledgement_catch_up ? "Authority ACK catch-up"
                                                              : "Authority hard resync",
                                     std::get<prediction::TimelineFailure>(resynced));
                log_authority_frontiers("Failed authority hard resync", candidate_world);
                return RuntimeError::PredictionTimelineFailed;
            }
            append_observable_event_batch(committed_batches, std::move(*commit));
        } else if (!rebuild && timeline_->authoritative_tick() < frame.tick) {
            scratch_timeline = *timeline_;
            auto reconciled = scratch_timeline.reconcile_with_stimulus_refresh(
                frame,
                [authority](mycore::rollback::CommandSequence,
                            const prediction::TickStimulus& previous,
                            const simulation::World& replay_state,
                            const prediction::PredictionScope& scope) {
                    return refresh_remote_assumptions(previous, replay_state, scope, *authority);
                });
            auto* commit = std::get_if<prediction::Commit>(&reconciled);
            if (commit == nullptr) {
                log_timeline_failure("Authority reconciliation",
                                     std::get<prediction::TimelineFailure>(reconciled));
                log_authority_frontiers("Failed authority reconciliation", candidate_world);
                return RuntimeError::PredictionTimelineFailed;
            }
            append_observable_event_batch(committed_batches, std::move(*commit));
        } else if (!rebuild && timeline_->authoritative_tick() == frame.tick) {
            scratch_timeline = *timeline_;
            auto refreshed = scratch_timeline.refresh_authority_with_stimulus_refresh(
                frame,
                [authority](mycore::rollback::CommandSequence,
                            const prediction::TickStimulus& previous,
                            const simulation::World& replay_state,
                            const prediction::PredictionScope& scope) {
                    return refresh_remote_assumptions(previous, replay_state, scope, *authority);
                });
            auto* commit = std::get_if<prediction::Commit>(&refreshed);
            if (commit == nullptr) {
                log_timeline_failure("Same-tick authority refresh",
                                     std::get<prediction::TimelineFailure>(refreshed));
                log_authority_frontiers("Failed same-tick authority refresh", candidate_world);
                return RuntimeError::PredictionTimelineFailed;
            }
            append_observable_event_batch(committed_batches, std::move(*commit));
        } else if (rebuild && timeline_ && confirmed_owner_id_ == *owner) {
            scratch_timeline = *timeline_;
            auto rebased = scratch_timeline.rebase_scope_with_stimulus_refresh(
                frame,
                selected_scope,
                [authority](mycore::rollback::CommandSequence,
                            const prediction::TickStimulus& previous,
                            const simulation::World& replay_state,
                            const prediction::PredictionScope& scope) {
                    return refresh_remote_assumptions(previous, replay_state, scope, *authority);
                });
            auto* commit = std::get_if<prediction::Commit>(&rebased);
            if (commit == nullptr) {
                const auto& failure = std::get<prediction::TimelineFailure>(rebased);
                log_timeline_failure("Scope rebase", failure);
                log_authority_frontiers("Failed scope rebase", candidate_world);
                return RuntimeError::PredictionTimelineFailed;
            }
            append_observable_event_batch(committed_batches, std::move(*commit));
        } else {
            scratch_timeline = prediction::Timeline{prediction::WorldModel{},
                                                    {.capacity = kPredictionHistoryCapacity}};
            auto initialized = scratch_timeline.initialize(frame, selected_scope);
            auto* commit = std::get_if<prediction::Commit>(&initialized);
            if (commit == nullptr) {
                log_timeline_failure("Timeline initialization",
                                     std::get<prediction::TimelineFailure>(initialized));
                return RuntimeError::PredictionTimelineFailed;
            }
            append_observable_event_batch(committed_batches, std::move(*commit));
        }

        auto last_timeline_sequence = candidate_world.last_processed_input_id();
        if (!scratch_timeline.history().empty()) {
            last_timeline_sequence = protocol::InputSequenceId{static_cast<std::uint32_t>(
                scratch_timeline.history().back().command_sequence.value())};
        }
        for (std::size_t index = 0; index < scratch_history.size(); ++index) {
            const auto& sample = scratch_history.at(index).sample;
            if (last_timeline_sequence.is_valid() && sample.sequence_id <= last_timeline_sequence) {
                continue;
            }
            auto advance_result = advance_timeline(scratch_timeline, sample, *owner, *authority);
            if (const auto* failure = std::get_if<prediction::TimelineFailure>(&advance_result)) {
                log_timeline_failure("Buffered input replay", *failure);
                return RuntimeError::PredictionTimelineFailed;
            }
            const auto* status = std::get_if<TimelineAdvanceStatus>(&advance_result);
            if (status != nullptr && *status == TimelineAdvanceStatus::StimulusUnavailable) {
                mycore::debug::log_error(
                    "dots.client.prediction",
                    "Buffered input replay could not refresh a remote movement assumption");
                return RuntimeError::PredictionTimelineFailed;
            }
            if (status != nullptr && *status == TimelineAdvanceStatus::Deferred) {
                break;
            }
            append_observable_event_batch(committed_batches,
                                          std::get<prediction::Commit>(std::move(advance_result)));
            last_timeline_sequence = sample.sequence_id;
        }

        const auto current_checkpoint = scratch_timeline.state()->checkpoint();
        auto remaps_result =
            prediction_identity_remaps(previous_checkpoint, current_checkpoint, *owner);
        auto* remaps = std::get_if<std::vector<PredictionIdentityRemap>>(&remaps_result);
        if (remaps == nullptr) {
            return std::get<RuntimeError>(remaps_result);
        }
        const auto projection =
            project_predicted_owner(*scratch_timeline.state(),
                                    *owner,
                                    candidate_world.recipient().primary_entity_id,
                                    previous_prediction);
        auto scratch_replay_path = replay_path(scratch_timeline, projection.primary_entity_id);
        const auto next_prediction = projection.position;
        const auto correction_displacement = previous_prediction && next_prediction
                                                 ? *previous_prediction - *next_prediction
                                                 : mycore::math::Vector2{};
        const auto correction_distance = mycore::math::length(correction_displacement);
        const auto nonzero_correction = correction_distance > kCorrectionTolerance;
        const auto remote_displacements =
            previous_checkpoint
                ? remote_player_displacements(*previous_checkpoint, current_checkpoint, *owner)
                : std::vector<RemotePlayerDisplacement>{};
        const auto comparable_remote_corrections =
            previous_checkpoint && previous_checkpoint->tick == current_checkpoint.tick;
        const auto maximum_remote_displacement =
            std::max_element(remote_displacements.begin(),
                             remote_displacements.end(),
                             [](const auto& lhs, const auto& rhs) {
                                 return lhs.distance < rhs.distance;
                             });

        auto scratch_retirements = pending_consequence_retirements_;
        attach_external_retirement_evidence(
            candidate_receipts, receipt_delta, scratch_retirements, committed_batches);
        if (prediction_event_batches_.size() + committed_batches.size() >
            kPredictionEventBatchCapacity) {
            return RuntimeError::PredictionEventQueueFull;
        }
        if (!pending_receipts.empty() &&
            !candidate_receipts.mark_published_through(pending_receipts.back().sequence_id)) {
            mycore::debug::log_error(
                "dots.client.prediction",
                "Authority receipt publication failed at sequence {} (accepted {}, published {})",
                pending_receipts.back().sequence_id.value(),
                candidate_receipts.accepted_through().value(),
                candidate_receipts.published_through().value());
            return RuntimeError::PredictionTimelineFailed;
        }
        timeline_ = std::move(scratch_timeline);
        for (auto& batch : committed_batches) {
            prediction_event_batches_.push_back(std::move(batch));
        }
        pending_consequence_retirements_ = std::move(scratch_retirements);
        latest_authority_checkpoint_ = *authority;
        input_history_ = scratch_history;
        confirmed_owner_id_ = *owner;
        predicted_owned_entity_ids_ = projection.owned_entity_ids;
        predicted_scope_entity_ids_ = scope_entity_ids(selected_scope);
        predicted_primary_entity_id_ = projection.primary_entity_id;
        predicted_position_ = next_prediction;
        prediction_debug_offset_ = {};
        latest_prediction_identity_remaps_ = std::move(*remaps);
        latest_replay_path_ = std::move(scratch_replay_path);
        if (acknowledgement_catch_up) {
            pre_correction_position_.reset();
            latest_correction_replay_path_.clear();
            recent_prediction_corrections_.clear();
            accumulated_correction_displacement_ = {};
            correction_sequence_since_hard_resync_ = 0;
            ++hard_resync_count_;
            ++acknowledgement_catch_up_count_;
            if (settings_.log_prediction_frontier_changes) {
                mycore::debug::log_info(
                    "dots.client.prediction.frontier",
                    "Authority ACK catch-up hard-resynced snapshot {} tick {}: ACK {}, prior "
                    "timeline submission {}, previously deferred {}, replayed unacknowledged {}",
                    candidate_world.snapshot_id().value(),
                    candidate_world.server_tick(),
                    sequence_name(candidate_world.last_processed_input_id()),
                    sequence_name(previous_timeline_submission),
                    previous_deferred_input_count,
                    input_history_.size());
            }
        } else if (settings_.log_prediction_frontier_changes && previous_deferred_input_count > 0 &&
                   deferred_prediction_input_count() == 0) {
            mycore::debug::log_info(
                "dots.client.prediction.frontier",
                "Prediction timeline resumed at snapshot {} tick {} after replaying {} deferred "
                "inputs; timeline submitted {}",
                candidate_world.snapshot_id().value(),
                candidate_world.server_tick(),
                previous_deferred_input_count,
                sequence_name(timeline_->last_submitted_sequence()));
        }
        if (previous_scope && previous_scope->scope_epoch != selected_scope.scope_epoch) {
            ++scope_rebase_count_;
            if (settings_.log_prediction_scope_changes) {
                mycore::debug::log_info(
                    "dots.client.prediction.scope",
                    "Scope rebase {} -> {} at snapshot {} tick {} for {} retained inputs: "
                    "horizon {} -> {}, owners {} -> {}, event owners {} -> {}, players {} -> {}, "
                    "food {} -> {}",
                    previous_scope->scope_epoch.value(),
                    selected_scope.scope_epoch.value(),
                    candidate_world.snapshot_id().value(),
                    candidate_world.server_tick(),
                    input_history_.size(),
                    previous_scope->replay_horizon.value(),
                    selected_scope.replay_horizon.value(),
                    previous_scope->owner_ids.size(),
                    selected_scope.owner_ids.size(),
                    previous_scope->subscribed_event_owner_ids.size(),
                    selected_scope.subscribed_event_owner_ids.size(),
                    previous_scope->player_ids.size(),
                    selected_scope.player_ids.size(),
                    previous_scope->food_ids.size(),
                    selected_scope.food_ids.size());
            }
        }
        if (settings_.log_prediction_reconciliation_details && previous_checkpoint &&
            maximum_remote_displacement != remote_displacements.end()) {
            const auto& displacement = *maximum_remote_displacement;
            mycore::debug::log_info(
                "dots.client.prediction.reconciliation",
                "{} at snapshot {} authority tick {} moved predicted head {} -> {}; remote "
                "entity {} ({:.3f}, {:.3f}) -> ({:.3f}, {:.3f}), delta {:.3f}, "
                "same-head-tick {}, retained inputs {}",
                reason == AuthorityInstallReason::ScopeRebase  ? "Scope rebase"
                : reason == AuthorityInstallReason::HardResync ? "Hard resync"
                                                               : "Authority reconciliation",
                candidate_world.snapshot_id().value(),
                candidate_world.server_tick(),
                previous_checkpoint->tick.value(),
                current_checkpoint.tick.value(),
                displacement.entity_id.value(),
                displacement.previous.x,
                displacement.previous.y,
                displacement.current.x,
                displacement.current.y,
                displacement.distance,
                comparable_remote_corrections ? "YES" : "NO",
                input_history_.size());
        }
        if (nonzero_correction && previous_prediction && next_prediction) {
            pre_correction_position_ = previous_prediction;
            latest_correction_replay_path_ = latest_replay_path_;
            accumulated_correction_displacement_ =
                accumulated_correction_displacement_ + correction_displacement;
            if (projection.primary_entity_id.is_valid()) {
                const auto mass = timeline_->state()->mass(
                    simulation::EntityId{projection.primary_entity_id.value()});
                if (mass) {
                    append_prediction_correction({
                        .entity_id = projection.primary_entity_id,
                        .pre_correction_position = *previous_prediction,
                        .corrected_position = *next_prediction,
                        .mass = *mass,
                        .distance = correction_distance,
                        .source = PredictionCorrectionSource::Local,
                    });
                }
            }
        }
        latest_remote_entity_correction_count_ = 0;
        latest_remote_correction_distance_ = 0.0F;
        if (comparable_remote_corrections) {
            for (const auto& displacement : remote_displacements) {
                append_prediction_correction({
                    .entity_id = protocol::EntityId{displacement.entity_id.value()},
                    .pre_correction_position = displacement.previous,
                    .corrected_position = displacement.current,
                    .mass = displacement.mass,
                    .distance = displacement.distance,
                    .source = PredictionCorrectionSource::Remote,
                });
                ++latest_remote_entity_correction_count_;
                ++remote_entity_correction_count_;
                latest_remote_correction_distance_ =
                    std::max(latest_remote_correction_distance_, displacement.distance);
                maximum_remote_correction_distance_ =
                    std::max(maximum_remote_correction_distance_, displacement.distance);
            }
        }

        const auto replay_duration = std::chrono::steady_clock::now() - replay_start;
        rollback_snapshot_id_ = candidate_world.snapshot_id();
        rollback_server_tick_ = candidate_world.server_tick();
        rollback_input_acknowledgement_ = candidate_world.last_processed_input_id();
        latest_replay_count_ = input_history_.size();
        total_replayed_input_count_ += latest_replay_count_;
        maximum_replay_count_ = std::max(maximum_replay_count_, latest_replay_count_);
        if (previous_checkpoint && reason == AuthorityInstallReason::NewAuthority) {
            ++reconciliation_count_;
        }
        latest_correction_distance_ = correction_distance;
        maximum_correction_distance_ = std::max(maximum_correction_distance_, correction_distance);
        prune_correction_times(now);
        if (nonzero_correction) {
            ++nonzero_correction_count_;
            ++correction_sequence_since_hard_resync_;
            correction_times_.push_back(now);
        }
        record_replay_duration(replay_duration, now);
        history_high_water_mark_ = std::max(history_high_water_mark_, input_history_.size());
        report_history_health(now);
        return std::nullopt;
    }

    [[nodiscard]] std::vector<mycore::math::Vector2>
    replay_path(const prediction::Timeline& timeline,
                protocol::EntityId preferred_entity_id) const {
        std::vector<mycore::math::Vector2> result;
        result.reserve(timeline.history().size());
        for (const auto& frame : timeline.history()) {
            const auto player =
                std::find_if(frame.checkpoint.players.begin(),
                             frame.checkpoint.players.end(),
                             [preferred_entity_id](const simulation::PlayerCheckpoint& value) {
                                 return value.entity_id.value() == preferred_entity_id.value();
                             });
            if (player != frame.checkpoint.players.end()) {
                result.push_back(player->position);
            }
        }
        return result;
    }

    [[nodiscard]] std::optional<RuntimeError>
    advance_prediction(const protocol::InputSample& sample) {
        if (!timeline_ || !latest_authority_checkpoint_ || !confirmed_owner_id_.is_valid()) {
            mycore::debug::log_error(
                "dots.client.prediction",
                "Local prediction advance is missing timeline, authority, or owner state");
            return RuntimeError::PredictionTimelineFailed;
        }
        auto result = advance_timeline(
            *timeline_, sample, confirmed_owner_id_, *latest_authority_checkpoint_);
        if (const auto* failure = std::get_if<prediction::TimelineFailure>(&result)) {
            log_timeline_failure("Local prediction advance", *failure);
            return RuntimeError::PredictionTimelineFailed;
        }
        const auto* status = std::get_if<TimelineAdvanceStatus>(&result);
        if (status != nullptr && *status == TimelineAdvanceStatus::StimulusUnavailable) {
            mycore::debug::log_error(
                "dots.client.prediction",
                "Local prediction advance could not refresh a remote movement assumption");
            return RuntimeError::PredictionTimelineFailed;
        }
        if (status != nullptr && *status == TimelineAdvanceStatus::Deferred) {
            if (settings_.log_prediction_frontier_changes &&
                deferred_prediction_input_count() == 1) {
                mycore::debug::log_info(
                    "dots.client.prediction.frontier",
                    "Prediction timeline deferred at input {} because predicted owner {} is "
                    "absent; retained inputs {}, timeline submitted {}",
                    sample.sequence_id.value(),
                    confirmed_owner_id_.value(),
                    input_history_.size(),
                    sequence_name(timeline_->last_submitted_sequence()));
            }
            return std::nullopt;
        }
        append_observable_event_batch(prediction_event_batches_,
                                      std::get<prediction::Commit>(std::move(result)));
        const auto projection = project_predicted_owner(*timeline_->state(),
                                                        confirmed_owner_id_,
                                                        world_.recipient().primary_entity_id,
                                                        predicted_position_);
        predicted_owned_entity_ids_ = projection.owned_entity_ids;
        predicted_primary_entity_id_ = projection.primary_entity_id;
        if (projection.position) {
            predicted_position_ = *projection.position + prediction_debug_offset_;
            input_history_.at(input_history_.size() - 1).resulting_position = *predicted_position_;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<RuntimeError>
    ensure_prediction_scope(std::size_t replay_input_count,
                            std::chrono::steady_clock::time_point now) {
        if (!latest_authority_snapshot_ || !world_rules_ || !timeline_ ||
            !confirmed_owner_id_.is_valid()) {
            mycore::debug::log_error(
                "dots.client.prediction",
                "Prediction scope validation is missing snapshot, rules, timeline, or owner state");
            return RuntimeError::PredictionTimelineFailed;
        }
        const auto hydrated =
            replication::hydrate_checkpoint(*latest_authority_snapshot_, *world_rules_);
        const auto* authority = std::get_if<simulation::WorldCheckpoint>(&hydrated);
        if (authority == nullptr) {
            return RuntimeError::CheckpointHydrationFailed;
        }
        auto required_result = required_scope(*authority,
                                              confirmed_owner_id_,
                                              timeline_->scope_epoch(),
                                              replay_horizon(replay_input_count));
        const auto* required = std::get_if<prediction::PredictionScope>(&required_result);
        if (required == nullptr) {
            return std::get<RuntimeError>(required_result);
        }
        if (timeline_->scope() && scope_covers(*timeline_->scope(), *required)) {
            return std::nullopt;
        }
        return install_authority(*latest_authority_snapshot_,
                                 world_,
                                 authority_receipts_,
                                 now,
                                 replay_input_count,
                                 AuthorityInstallReason::ScopeRebase);
    }

    [[nodiscard]] bool hard_resync(std::chrono::steady_clock::time_point now) {
        if (!latest_authority_snapshot_ || !world_rules_) {
            return false;
        }
        auto previous_history = input_history_;
        input_history_.clear();
        if (const auto error = install_authority(*latest_authority_snapshot_,
                                                 world_,
                                                 authority_receipts_,
                                                 now,
                                                 0,
                                                 AuthorityInstallReason::HardResync)) {
            input_history_ = previous_history;
            return false;
        }
        pre_correction_position_.reset();
        latest_replay_path_.clear();
        latest_correction_replay_path_.clear();
        recent_prediction_corrections_.clear();
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

    void clear_prediction_state() noexcept {
        input_history_.clear();
        timeline_.reset();
        latest_authority_checkpoint_.reset();
        confirmed_owner_id_ = {};
        predicted_primary_entity_id_ = {};
        predicted_owned_entity_ids_.clear();
        predicted_scope_entity_ids_.clear();
        latest_prediction_identity_remaps_.clear();
        recent_prediction_corrections_.clear();
        predicted_position_.reset();
        prediction_debug_offset_ = {};
        pre_correction_position_.reset();
        latest_replay_path_.clear();
        latest_correction_replay_path_.clear();
        accumulated_correction_displacement_ = {};
        correction_sequence_since_hard_resync_ = 0;
    }

    void append_prediction_correction(PredictionCorrection correction) {
        if (recent_prediction_corrections_.size() == kRecentPredictionCorrectionCapacity) {
            recent_prediction_corrections_.erase(recent_prediction_corrections_.begin());
        }
        correction.sequence = ++prediction_correction_sequence_;
        recent_prediction_corrections_.push_back(correction);
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
        if (!client_id_.is_valid() || !world_rules_ || !world_.snapshot_id().is_valid()) {
            return std::nullopt;
        }
        const auto session_mode = world_.recipient().mode;
        if (!valid_respawn_deadline(world_.recipient())) {
            return fail(RuntimeError::InvalidSnapshot);
        }
        const auto primary_entity_id = world_.recipient().primary_entity_id;
        const auto* controlled = world_.find(primary_entity_id);
        if (session_mode == protocol::SessionMode::Playing &&
            (controlled == nullptr || controlled->kind != protocol::EntityKind::Player ||
             !timeline_ || !predicted_position_)) {
            return fail(RuntimeError::MissingControlledEntity);
        }
        if (state_ != State::Ready) {
            state_ = State::Ready;
            mycore::debug::log_info("dots.client.session",
                                    "Session ready as client {} with primary entity {}",
                                    client_id_.value(),
                                    primary_entity_id.value());
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
    std::uint32_t respawn_cooldown_ticks_{};
    std::optional<protocol::WorldRules> world_rules_;
    replication::ReplicatedWorld world_;
    replication::AuthorityReceiptInbox authority_receipts_;
    std::optional<protocol::FullSnapshot> latest_authority_snapshot_;
    std::uint32_t next_input_id_{};
    std::optional<std::uint32_t> last_sent_client_tick_;
    PredictionHistory input_history_;
    std::optional<prediction::Timeline> timeline_;
    std::deque<PredictionEventBatch> prediction_event_batches_;
    std::map<simulation::SimulationEventKey, mycore::time::Tick> pending_consequence_retirements_;
    std::optional<simulation::WorldCheckpoint> latest_authority_checkpoint_;
    simulation::PlayerOwnerId confirmed_owner_id_;
    protocol::EntityId predicted_primary_entity_id_;
    std::vector<protocol::EntityId> predicted_owned_entity_ids_;
    std::vector<protocol::EntityId> predicted_scope_entity_ids_;
    std::vector<PredictionIdentityRemap> latest_prediction_identity_remaps_;
    std::vector<PredictionCorrection> recent_prediction_corrections_;
    std::optional<mycore::math::Vector2> predicted_position_;
    mycore::math::Vector2 prediction_debug_offset_;
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
    std::uint64_t scope_rebase_count_{};
    std::uint64_t nonzero_correction_count_{};
    std::uint64_t remote_entity_correction_count_{};
    std::size_t latest_remote_entity_correction_count_{};
    float latest_correction_distance_{};
    float maximum_correction_distance_{};
    float latest_remote_correction_distance_{};
    float maximum_remote_correction_distance_{};
    mycore::math::Vector2 accumulated_correction_displacement_;
    std::uint64_t correction_sequence_since_hard_resync_{};
    std::uint64_t prediction_correction_sequence_{};
    std::deque<std::chrono::steady_clock::time_point> correction_times_;
    std::uint64_t replay_over_budget_count_{};
    std::uint64_t hard_resync_count_{};
    std::uint64_t acknowledgement_catch_up_count_{};
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

InputSendResult Runtime::send_input(std::uint32_t client_tick,
                                    mycore::math::Vector2 movement,
                                    std::uint16_t action_bits) {
    return impl_->send_input(client_tick, movement, action_bits);
}

bool Runtime::disconnect() {
    return impl_->disconnect();
}

std::vector<PredictionEventBatch> Runtime::take_prediction_event_batches() {
    return impl_->take_prediction_event_batches();
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

protocol::SessionMode Runtime::session_mode() const noexcept {
    return impl_->session_mode();
}

std::span<const protocol::EntityId> Runtime::owned_entity_ids() const noexcept {
    return impl_->owned_entity_ids();
}

protocol::EntityId Runtime::primary_entity_id() const noexcept {
    return impl_->primary_entity_id();
}

protocol::EntityId Runtime::follow_entity_id() const noexcept {
    return impl_->follow_entity_id();
}

std::optional<protocol::WorldRules> Runtime::world_rules() const noexcept {
    return impl_->world_rules();
}

std::optional<std::uint32_t> Runtime::defeat_tick() const noexcept {
    return impl_->defeat_tick();
}

std::optional<std::uint32_t> Runtime::respawn_available_tick() const noexcept {
    return impl_->respawn_available_tick();
}

std::uint32_t Runtime::respawn_cooldown_ticks() const noexcept {
    return impl_->respawn_cooldown_ticks();
}

std::optional<protocol::PlayerAbsorbed> Runtime::latest_absorption() const noexcept {
    return impl_->latest_absorption();
}

protocol::InputSequenceId Runtime::latest_respawn_request_id() const noexcept {
    return impl_->latest_respawn_request_id();
}

protocol::RespawnResult Runtime::latest_respawn_result() const noexcept {
    return impl_->latest_respawn_result();
}

mycore::net_transport::ConnectionHandle Runtime::connection_handle() const noexcept {
    return impl_->connection_handle();
}

const simulation::World* Runtime::predicted_world() const noexcept {
    return impl_->predicted_world();
}

protocol::EntityId Runtime::predicted_primary_entity_id() const noexcept {
    return impl_->predicted_primary_entity_id();
}

std::span<const protocol::EntityId> Runtime::predicted_owned_entity_ids() const noexcept {
    return impl_->predicted_owned_entity_ids();
}

std::span<const protocol::EntityId> Runtime::predicted_scope_entity_ids() const noexcept {
    return impl_->predicted_scope_entity_ids();
}

std::span<const PredictionIdentityRemap>
Runtime::latest_prediction_identity_remaps() const noexcept {
    return impl_->latest_prediction_identity_remaps();
}

std::span<const PredictionCorrection> Runtime::recent_prediction_corrections() const noexcept {
    return impl_->recent_prediction_corrections();
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

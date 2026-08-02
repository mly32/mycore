#include "dots/server/server_runtime.hpp"

#include "dots/protocol/codec.hpp"
#include "dots/replication/replication.hpp"
#include "dots/simulation/world_setup.hpp"
#include "mycore/debug/log.hpp"
#include "mycore/math/vector2.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <string>
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
    protocol::JoinRole role{protocol::JoinRole::Player};
    simulation::PlayerOwnerId owner_id;
    protocol::SessionMode mode{protocol::SessionMode::Playing};
    std::vector<simulation::EntityId> player_ids;
    simulation::EntityId primary_player_id;
    simulation::EntityId follow_player_id;
    std::optional<std::uint32_t> defeat_tick;
    std::optional<std::uint32_t> respawn_available_tick;
    std::optional<protocol::PlayerAbsorbed> latest_absorption;
    protocol::InputSequenceId latest_respawn_request_id;
    protocol::RespawnResult latest_respawn_result{protocol::RespawnResult::None};
    protocol::InputSequenceId last_processed_input_id;
    std::map<std::uint32_t, protocol::InputSample> pending_inputs;
    std::uint64_t last_activity_tick{};
    std::uint32_t consecutive_missing_input_ticks{};
    std::uint32_t next_snapshot_id{};
    protocol::AuthorityReceiptSequenceId last_acknowledged_authority_receipt;
    std::deque<protocol::AuthorityReceipt> pending_authority_receipts;
    std::uint32_t next_authority_receipt_sequence{};
    std::size_t pending_input_high_water_mark{};
    bool input_pressure_active{};

    [[nodiscard]] bool ready() const noexcept {
        return client_id.is_valid();
    }
};

enum class InputEnqueueResult : std::uint8_t {
    Accepted,
    ConflictingDuplicate,
    OutsideReceiveWindow,
    Overflow,
};

enum class ReceiptAcknowledgementResult : std::uint8_t {
    Accepted,
    Invalid,
};

} // namespace

class Runtime::Impl {
public:
    Impl(mycore::net_transport::Endpoint& endpoint,
         simulation::World initial_world,
         RuntimeSettings settings)
        : endpoint_(endpoint),
          world_(std::move(initial_world)),
          settings_(settings) {}

    [[nodiscard]] std::optional<RuntimeError> process_events() {
        for (const auto& event : endpoint_.poll()) {
            if (const auto* connected = std::get_if<mycore::net_transport::Connected>(&event)) {
                const auto [unused, inserted] = sessions_.try_emplace(
                    connected->connection.value(),
                    Session{
                        .connection = connected->connection,
                        .client_id = {},
                        .role = protocol::JoinRole::Player,
                        .owner_id = {},
                        .mode = protocol::SessionMode::Playing,
                        .player_ids = {},
                        .primary_player_id = {},
                        .follow_player_id = {},
                        .defeat_tick = {},
                        .respawn_available_tick = {},
                        .latest_absorption = {},
                        .latest_respawn_request_id = {},
                        .latest_respawn_result = protocol::RespawnResult::None,
                        .last_processed_input_id = {},
                        .pending_inputs = {},
                        .last_activity_tick = world_.tick().value(),
                        .consecutive_missing_input_ticks = 0,
                        .next_snapshot_id = 0,
                        .last_acknowledged_authority_receipt = {},
                        .pending_authority_receipts = {},
                        .next_authority_receipt_sequence = 0,
                        .pending_input_high_water_mark = 0,
                        .input_pressure_active = false,
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
                if (const auto error =
                        accept(session_iterator->second,
                               std::get<protocol::ClientHello>(*message).requested_role)) {
                    return error;
                }
                continue;
            }

            if (const auto* status = std::get_if<protocol::ClientStatus>(message)) {
                auto& session = session_iterator->second;
                if (received.delivery != DeliveryMode::Unreliable || !session.ready()) {
                    reject(received.connection);
                    continue;
                }
                if (acknowledge_authority_receipts(
                        session, status->last_received_authority_receipt_sequence) ==
                    ReceiptAcknowledgementResult::Invalid) {
                    reject(received.connection, "invalid authority receipt acknowledgement");
                    continue;
                }
                session.last_activity_tick = world_.tick().value();
                continue;
            }

            const auto* input = std::get_if<protocol::InputPacket>(message);
            if (input == nullptr || received.delivery != DeliveryMode::Unreliable ||
                !session_iterator->second.ready()) {
                reject(received.connection);
                continue;
            }
            auto& session = session_iterator->second;
            if (session.role != protocol::JoinRole::Player) {
                reject(received.connection, "gameplay input from spectator-only session");
                continue;
            }
            if (acknowledge_authority_receipts(session,
                                               input->last_received_authority_receipt_sequence) ==
                ReceiptAcknowledgementResult::Invalid) {
                reject(received.connection, "invalid authority receipt acknowledgement");
                continue;
            }
            const auto enqueue_result = enqueue_input(session, *input);
            if (enqueue_result == InputEnqueueResult::Accepted) {
                session.last_activity_tick = world_.tick().value();
                session.pending_input_high_water_mark =
                    std::max(session.pending_input_high_water_mark, session.pending_inputs.size());
                if (!session.input_pressure_active && session.pending_inputs.size() >= 24) {
                    session.input_pressure_active = true;
                    mycore::debug::log_warning(
                        "dots.server.input",
                        "Client {} input queue entered pressure at depth {}; processed {}, "
                        "grant {}, high-water {}",
                        session.client_id.value(),
                        session.pending_inputs.size(),
                        sequence_name(session.last_processed_input_id),
                        sequence_name(input_receive_through(session)),
                        session.pending_input_high_water_mark);
                }
            }
            if (enqueue_result == InputEnqueueResult::ConflictingDuplicate) {
                reject(received.connection, "conflicting duplicate input sample");
                continue;
            }
            if (enqueue_result == InputEnqueueResult::OutsideReceiveWindow) {
                const auto receive_through = input_receive_through(session);
                mycore::debug::log_warning(
                    "dots.server.input",
                    "Rejected input outside receive window for client {} on connection {}: "
                    "processed {}, grant {}, queue {}/{}, packet [{}..{}]",
                    session.client_id.value(),
                    received.connection.value(),
                    sequence_name(session.last_processed_input_id),
                    sequence_name(receive_through),
                    session.pending_inputs.size(),
                    session.pending_input_high_water_mark,
                    input->samples.front().sequence_id.value(),
                    input->samples.back().sequence_id.value());
                reject(received.connection, "input outside receive window");
                continue;
            }
            if (enqueue_result == InputEnqueueResult::Overflow) {
                reject(received.connection, "pending input queue overflow");
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<RuntimeError> step() {
        remove_inactive_sessions();
        std::vector<std::pair<std::uint32_t, protocol::InputSequenceId>> applied_inputs;
        applied_inputs.reserve(sessions_.size());
        std::vector<simulation::TickCommand> tick_commands;
        tick_commands.reserve(sessions_.size());
        for (auto& [connection_value, session] : sessions_) {
            if (!session.ready()) {
                continue;
            }
            if (session.pending_inputs.empty()) {
                if (session.mode == protocol::SessionMode::Spectating) {
                    continue;
                }
                ++session.consecutive_missing_input_ticks;
                if (session.consecutive_missing_input_ticks > settings_.input_hold_ticks) {
                    tick_commands.push_back({
                        .type = simulation::TickCommandType::StopMovement,
                        .input_id = simulation::InputCommandId::invalid(),
                        .owner_id = session.owner_id,
                        .movement = {},
                    });
                }
                continue;
            }
            const auto& sample = session.pending_inputs.begin()->second;
            if ((sample.action_bits & protocol::kRespawnActionBit) != 0U) {
                if (const auto error = process_respawn_request(session, sample.sequence_id)) {
                    return error;
                }
            }
            if (session.mode == protocol::SessionMode::Playing) {
                if (!session.primary_player_id.is_valid()) {
                    return RuntimeError::SimulationInputRejected;
                }
                tick_commands.push_back({
                    .type = simulation::TickCommandType::ApplyInput,
                    .input_id = replication::to_simulation(sample.sequence_id),
                    .owner_id = session.owner_id,
                    .movement = {sample.movement_x, sample.movement_y},
                    .split_requested = (sample.action_bits & protocol::kSplitActionBit) != 0U,
                });
            }
            session.consecutive_missing_input_ticks = 0;
            applied_inputs.emplace_back(connection_value, sample.sequence_id);
        }

        const auto tick_result = world_.advance(tick_commands);
        const auto* journal = std::get_if<simulation::TickJournal>(&tick_result);
        if (journal == nullptr) {
            return std::get<simulation::TickError>(tick_result) ==
                           simulation::TickError::SimulationRejected
                       ? RuntimeError::SimulationStepFailed
                       : RuntimeError::SimulationInputRejected;
        }
        if (const auto error = process_simulation_events(*journal)) {
            return error;
        }
        for (const auto& [connection_value, sequence_id] : applied_inputs) {
            auto& session = sessions_.at(connection_value);
            session.last_processed_input_id = sequence_id;
            session.pending_inputs.erase(sequence_id.value());
            if (session.input_pressure_active && session.pending_inputs.size() <= 8) {
                session.input_pressure_active = false;
                mycore::debug::log_info(
                    "dots.server.input",
                    "Client {} input queue recovered to depth {}; processed {}, grant {}, "
                    "high-water {}",
                    session.client_id.value(),
                    session.pending_inputs.size(),
                    sequence_name(session.last_processed_input_id),
                    sequence_name(input_receive_through(session)),
                    session.pending_input_high_water_mark);
            }
        }
        refresh_follow_targets();
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
    [[nodiscard]] static std::string sequence_name(protocol::InputSequenceId sequence) {
        return sequence.is_valid() ? std::to_string(sequence.value()) : "none";
    }

    [[nodiscard]] static protocol::InputSequenceId
    input_receive_through(const Session& session) noexcept {
        if (session.role != protocol::JoinRole::Player) {
            return {};
        }
        return protocol::input_receive_through_for(session.last_processed_input_id);
    }

    [[nodiscard]] static ReceiptAcknowledgementResult
    acknowledge_authority_receipts(Session& session,
                                   protocol::AuthorityReceiptSequenceId acknowledgement) {
        if (!acknowledgement.is_valid()) {
            return ReceiptAcknowledgementResult::Accepted;
        }
        if (session.next_authority_receipt_sequence == 0 ||
            acknowledgement.value() >= session.next_authority_receipt_sequence) {
            return ReceiptAcknowledgementResult::Invalid;
        }
        if (session.last_acknowledged_authority_receipt.is_valid() &&
            acknowledgement <= session.last_acknowledged_authority_receipt) {
            return ReceiptAcknowledgementResult::Accepted;
        }
        while (!session.pending_authority_receipts.empty() &&
               session.pending_authority_receipts.front().sequence_id <= acknowledgement) {
            session.pending_authority_receipts.pop_front();
        }
        session.last_acknowledged_authority_receipt = acknowledgement;
        return ReceiptAcknowledgementResult::Accepted;
    }

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

        const auto receive_through = input_receive_through(session);
        if (!receive_through.is_valid() ||
            std::any_of(
                fresh_samples.begin(), fresh_samples.end(), [receive_through](const auto& sample) {
                    return sample.sequence_id > receive_through;
                })) {
            return InputEnqueueResult::OutsideReceiveWindow;
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

    [[nodiscard]] protocol::RecipientSessionState recipient_state(const Session& session) const {
        protocol::RecipientSessionState state{
            .mode = session.mode,
            .owned_entity_ids = {},
            .primary_entity_id = replication::to_protocol(session.primary_player_id),
            .follow_entity_id = replication::to_protocol(session.follow_player_id),
            .defeat_tick = session.defeat_tick,
            .respawn_available_tick = session.respawn_available_tick,
            .latest_absorption = session.latest_absorption,
            .latest_respawn_request_id = session.latest_respawn_request_id,
            .latest_respawn_result = session.latest_respawn_result,
        };
        state.owned_entity_ids.reserve(session.player_ids.size());
        for (const auto player_id : session.player_ids) {
            state.owned_entity_ids.push_back(replication::to_protocol(player_id));
        }
        std::sort(state.owned_entity_ids.begin(), state.owned_entity_ids.end());
        return state;
    }

    [[nodiscard]] Session* session_for_owner(simulation::PlayerOwnerId owner_id) noexcept {
        for (auto& [unused, session] : sessions_) {
            static_cast<void>(unused);
            if (session.ready() && session.owner_id == owner_id) {
                return &session;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<simulation::PlayerOwnerId> next_available_owner_id() {
        while (next_owner_id_ != simulation::PlayerOwnerId::kInvalidValue) {
            const auto candidate = simulation::PlayerOwnerId{next_owner_id_};
            const auto used_by_session = session_for_owner(candidate) != nullptr;
            const auto used_by_world =
                std::any_of(world_.player_ids().begin(),
                            world_.player_ids().end(),
                            [this, candidate](simulation::EntityId player_id) {
                                return world_.player_owner(player_id) == candidate;
                            });
            if (!used_by_session && !used_by_world) {
                return candidate;
            }
            ++next_owner_id_;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<RuntimeError>
    process_respawn_request(Session& session, protocol::InputSequenceId sequence_id) {
        session.latest_respawn_request_id = sequence_id;
        if (session.mode != protocol::SessionMode::Spectating) {
            session.latest_respawn_result = protocol::RespawnResult::RejectedNotSpectating;
            mycore::debug::log_info("dots.server.session",
                                    "Client {} respawn input {} rejected: session is not "
                                    "spectating",
                                    session.client_id.value(),
                                    sequence_id.value());
            return std::nullopt;
        }
        if (!session.respawn_available_tick) {
            return RuntimeError::InvalidWorldState;
        }
        if (world_.tick().value() < *session.respawn_available_tick) {
            session.latest_respawn_result = protocol::RespawnResult::RejectedCooldown;
            mycore::debug::log_info(
                "dots.server.session",
                "Client {} respawn input {} rejected at tick {}: cooldown ends at tick {}",
                session.client_id.value(),
                sequence_id.value(),
                world_.tick().value(),
                *session.respawn_available_tick);
            return std::nullopt;
        }

        const auto spawn_result = simulation::spawn_player_safely(world_, session.owner_id);
        const auto* player = std::get_if<simulation::EntityId>(&spawn_result);
        if (player == nullptr) {
            if (std::get<simulation::SafePlayerSpawnError>(spawn_result) ==
                simulation::SafePlayerSpawnError::NoSafePosition) {
                session.latest_respawn_result = protocol::RespawnResult::RejectedNoSafeSpawn;
                mycore::debug::log_warning(
                    "dots.server.session",
                    "Client {} respawn input {} rejected: no safe spawn is available",
                    session.client_id.value(),
                    sequence_id.value());
                return std::nullopt;
            }
            return RuntimeError::EntityIdExhausted;
        }

        session.mode = protocol::SessionMode::Playing;
        session.player_ids = {*player};
        session.primary_player_id = *player;
        session.follow_player_id = {};
        session.defeat_tick.reset();
        session.respawn_available_tick.reset();
        session.latest_respawn_result = protocol::RespawnResult::Accepted;
        mycore::debug::log_info("dots.server.session",
                                "Client {} respawned as entity {} for input {}",
                                session.client_id.value(),
                                player->value(),
                                sequence_id.value());
        return std::nullopt;
    }

    [[nodiscard]] bool enqueue_authority_receipt(Session& session,
                                                 const protocol::AuthorityEvent& event) {
        if (session.pending_authority_receipts.size() >=
                protocol::kMaximumPendingAuthorityReceipts ||
            session.next_authority_receipt_sequence ==
                protocol::AuthorityReceiptSequenceId::kInvalidValue) {
            return false;
        }
        session.pending_authority_receipts.push_back({
            .sequence_id =
                protocol::AuthorityReceiptSequenceId{session.next_authority_receipt_sequence},
            .event = event,
        });
        ++session.next_authority_receipt_sequence;
        return true;
    }

    [[nodiscard]] std::optional<RuntimeError>
    process_simulation_events(const simulation::TickJournal& journal) {
        std::vector<ConnectionHandle> receipt_failures;
        for (const auto& simulation_event : journal.events) {
            const auto converted = replication::to_protocol(simulation_event);
            const auto* authority_event = std::get_if<protocol::AuthorityEvent>(&converted);
            if (authority_event == nullptr) {
                return RuntimeError::TickOutOfRange;
            }

            std::vector<Session*> recipients;
            const auto add_recipient = [&recipients](Session* session) {
                if (session != nullptr &&
                    std::find(recipients.begin(), recipients.end(), session) == recipients.end()) {
                    recipients.push_back(session);
                }
            };
            if (std::holds_alternative<simulation::FoodConsumed>(simulation_event)) {
                // Food has no additional session lifecycle state.
            } else if (const auto* split_event =
                           std::get_if<simulation::PlayerSplit>(&simulation_event)) {
                auto* owner = session_for_owner(split_event->owner_id);
                if (owner != nullptr) {
                    owner->player_ids.push_back(split_event->child_entity_id);
                    std::sort(owner->player_ids.begin(), owner->player_ids.end());
                }
            } else if (const auto* merge_event =
                           std::get_if<simulation::PiecesMerged>(&simulation_event)) {
                auto* owner = session_for_owner(merge_event->owner_id);
                if (owner != nullptr) {
                    std::erase(owner->player_ids, merge_event->consumed_entity_id);
                    if (owner->primary_player_id == merge_event->consumed_entity_id) {
                        owner->primary_player_id = merge_event->survivor_entity_id;
                    }
                }
            } else {
                const auto& absorption_event =
                    std::get<simulation::PlayerAbsorbed>(simulation_event);
                if (absorption_event.tick.value() >= std::numeric_limits<std::uint32_t>::max()) {
                    return RuntimeError::TickOutOfRange;
                }
                const auto defeat_tick = static_cast<std::uint32_t>(absorption_event.tick.value());
                if (settings_.respawn_cooldown_ticks >
                    std::numeric_limits<std::uint32_t>::max() - defeat_tick) {
                    return RuntimeError::TickOutOfRange;
                }
                const auto& replicated_event = std::get<protocol::PlayerAbsorbed>(*authority_event);
                auto* absorber = session_for_owner(absorption_event.absorber_owner_id);
                if (absorber != nullptr) {
                    absorber->latest_absorption = replicated_event;
                }
                auto* victim = session_for_owner(absorption_event.victim_owner_id);
                if (victim != nullptr) {
                    victim->latest_absorption = replicated_event;
                    std::erase(victim->player_ids, absorption_event.victim_entity_id);
                    if (!victim->player_ids.empty()) {
                        if (victim->primary_player_id == absorption_event.victim_entity_id) {
                            victim->primary_player_id = victim->player_ids.front();
                        }
                    } else {
                        victim->mode = protocol::SessionMode::Spectating;
                        victim->primary_player_id = {};
                        victim->follow_player_id = absorption_event.absorber_entity_id;
                        victim->defeat_tick = defeat_tick;
                        victim->respawn_available_tick =
                            defeat_tick + settings_.respawn_cooldown_ticks;
                        victim->consecutive_missing_input_ticks = 0;
                        mycore::debug::log_info(
                            "dots.server.session",
                            "Client {} entered spectating after entity {} was absorbed by entity "
                            "{}; respawn available at tick {}",
                            victim->client_id.value(),
                            absorption_event.victim_entity_id.value(),
                            absorption_event.absorber_entity_id.value(),
                            *victim->respawn_available_tick);
                    }
                }
            }

            const auto participants = simulation::simulation_event_participants(simulation_event);
            for (const auto owner_id : participants.owners()) {
                add_recipient(session_for_owner(owner_id));
            }
            for (auto* recipient : recipients) {
                if (!enqueue_authority_receipt(*recipient, *authority_event)) {
                    receipt_failures.push_back(recipient->connection);
                }
            }
        }
        std::sort(receipt_failures.begin(), receipt_failures.end());
        receipt_failures.erase(std::unique(receipt_failures.begin(), receipt_failures.end()),
                               receipt_failures.end());
        for (const auto connection : receipt_failures) {
            if (sessions_.contains(connection.value())) {
                fail_session(connection, "authority receipt retention overflow");
            }
        }
        return std::nullopt;
    }

    void refresh_follow_targets() {
        for (auto& [unused, session] : sessions_) {
            static_cast<void>(unused);
            if (session.mode == protocol::SessionMode::Spectating &&
                session.follow_player_id.is_valid() && !world_.position(session.follow_player_id)) {
                mycore::debug::log_info(
                    "dots.server.session",
                    "Client {} follow entity {} is no longer present; clearing confirmed target",
                    session.client_id.value(),
                    session.follow_player_id.value());
                session.follow_player_id = {};
            }
        }
    }

    [[nodiscard]] std::optional<RuntimeError> accept(Session& session, protocol::JoinRole role) {
        if (world_.tick().value() > std::numeric_limits<std::uint32_t>::max()) {
            return RuntimeError::TickOutOfRange;
        }
        if (next_client_id_ == protocol::ClientId::kInvalidValue) {
            return RuntimeError::ClientIdExhausted;
        }
        auto owner_id = simulation::PlayerOwnerId{};
        auto player = simulation::EntityId{};
        if (role == protocol::JoinRole::Player) {
            owner_id = next_available_owner_id().value_or(simulation::PlayerOwnerId{});
            if (!owner_id.is_valid()) {
                return RuntimeError::PlayerOwnerIdExhausted;
            }
            const auto spawn_result = simulation::spawn_player_safely(world_, owner_id);
            const auto* spawned_player = std::get_if<simulation::EntityId>(&spawn_result);
            if (spawned_player == nullptr) {
                if (std::get<simulation::SafePlayerSpawnError>(spawn_result) ==
                    simulation::SafePlayerSpawnError::NoSafePosition) {
                    return RuntimeError::NoSafeSpawn;
                }
                return RuntimeError::EntityIdExhausted;
            }
            player = *spawned_player;
        }

        session.client_id = protocol::ClientId{next_client_id_++};
        session.role = role;
        if (role == protocol::JoinRole::Player) {
            session.owner_id = owner_id;
            ++next_owner_id_;
            session.mode = protocol::SessionMode::Playing;
            session.player_ids = {player};
            session.primary_player_id = player;
        } else {
            session.mode = protocol::SessionMode::Spectating;
        }
        session.last_activity_tick = world_.tick().value();
        const auto connection = session.connection;
        const protocol::ServerWelcome welcome{
            .client_id = session.client_id,
            .accepted_role = role,
            .server_tick = static_cast<std::uint32_t>(world_.tick().value()),
            .respawn_cooldown_ticks = settings_.respawn_cooldown_ticks,
            .world_rules = replication::to_protocol(world_.rules()),
        };
        if (const auto error = transmit(connection, welcome, DeliveryMode::Reliable)) {
            return error;
        }
        if (sessions_.contains(connection.value())) {
            const auto snapshot_error = send_snapshot(connection);
            if (!snapshot_error && sessions_.contains(connection.value())) {
                if (role == protocol::JoinRole::Spectator) {
                    mycore::debug::log_info("dots.server.session",
                                            "Spectator client {} joined on connection {}",
                                            session.client_id.value(),
                                            connection.value());
                } else if (const auto position = world_.position(session.primary_player_id)) {
                    mycore::debug::log_info("dots.server.session",
                                            "Client {} joined on connection {} controlling entity "
                                            "{} at ({:.1f}, {:.1f})",
                                            session.client_id.value(),
                                            connection.value(),
                                            session.primary_player_id.value(),
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
        const auto receipt_count = std::min(session.pending_authority_receipts.size(),
                                            protocol::kMaximumAuthorityReceiptsPerSnapshot);
        std::vector<protocol::AuthorityReceipt> authority_receipts;
        authority_receipts.reserve(receipt_count);
        std::copy_n(session.pending_authority_receipts.begin(),
                    receipt_count,
                    std::back_inserter(authority_receipts));
        const auto snapshot = replication::build_full_snapshot(
            world_,
            protocol::SnapshotId{session.next_snapshot_id},
            session.last_processed_input_id,
            static_cast<std::uint8_t>(session.pending_inputs.size()),
            recipient_state(session),
            std::move(authority_receipts),
            session.last_acknowledged_authority_receipt,
            input_receive_through(session));
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
            static_cast<void>(endpoint_.disconnect(connection));
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

    void fail_session(ConnectionHandle connection, std::string_view reason) {
        mycore::debug::log_warning("dots.server.session",
                                   "Session failure on connection {}: {}; disconnecting peer",
                                   connection.value(),
                                   reason);
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
                                "Transport connection {} closed with no active session ({})",
                                disconnected.connection.value(),
                                disconnect_reason_name(disconnected.reason));
    }

    void remove_session(ConnectionHandle connection) {
        const auto iterator = sessions_.find(connection.value());
        if (iterator == sessions_.end()) {
            return;
        }
        for (const auto player_id : iterator->second.player_ids) {
            static_cast<void>(world_.remove_player(player_id));
        }
        sessions_.erase(iterator);
    }

    void remove_inactive_sessions() {
        const auto current_tick = world_.tick().value();
        std::vector<ConnectionHandle> inactive_connections;
        inactive_connections.reserve(sessions_.size());
        for (const auto& [unused, session] : sessions_) {
            static_cast<void>(unused);
            const auto timeout_ticks = session.ready() ? settings_.liveness_timeout_ticks
                                                       : settings_.handshake_timeout_ticks;
            if (current_tick - session.last_activity_tick >= timeout_ticks) {
                inactive_connections.push_back(session.connection);
            }
        }
        for (const auto connection : inactive_connections) {
            const auto iterator = sessions_.find(connection.value());
            if (iterator == sessions_.end()) {
                continue;
            }
            if (iterator->second.ready()) {
                mycore::debug::log_warning(
                    "dots.server.session",
                    "Client {} timed out after {} server ticks without valid input; disconnecting",
                    iterator->second.client_id.value(),
                    settings_.liveness_timeout_ticks);
            } else {
                mycore::debug::log_warning(
                    "dots.server.session",
                    "Connection {} timed out after {} server ticks without completing the "
                    "handshake; disconnecting",
                    connection.value(),
                    settings_.handshake_timeout_ticks);
            }
            static_cast<void>(endpoint_.disconnect(connection));
            remove_session(connection);
        }
    }

    mycore::net_transport::Endpoint& endpoint_;
    simulation::World world_;
    RuntimeSettings settings_;
    std::unordered_map<std::uint32_t, Session> sessions_;
    std::uint32_t next_client_id_{};
    std::uint32_t next_owner_id_{};
    std::size_t rejected_packet_count_{};
};

Runtime::Runtime(mycore::net_transport::Endpoint& endpoint,
                 simulation::World initial_world,
                 RuntimeSettings settings)
    : impl_(std::make_unique<Impl>(endpoint, std::move(initial_world), settings)) {}

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

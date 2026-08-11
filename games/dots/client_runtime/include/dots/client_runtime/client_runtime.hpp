#pragma once

#include "dots/prediction/model.hpp"
#include "dots/protocol/ids.hpp"
#include "dots/protocol/messages.hpp"
#include "dots/replication/replication.hpp"
#include "dots/simulation/world.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/net_transport/net_transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace dots::client_runtime {

enum class State : std::uint8_t {
    Connecting,
    Handshaking,
    Ready,
    Disconnected,
    Failed,
};

enum class RuntimeError : std::uint8_t {
    MultipleConnections,
    ProtocolEncodeFailed,
    ProtocolDecodeFailed,
    UnexpectedMessage,
    InvalidSnapshot,
    InvalidInputAcknowledgement,
    MissingControlledEntity,
    CheckpointHydrationFailed,
    PredictionScopeFailed,
    PredictionTimelineFailed,
    PredictionConsequenceFailed,
    PredictionEventQueueFull,
    AmbiguousPredictionIdentity,
    TransportSendFailed,
};

[[nodiscard]] std::string_view runtime_error_name(RuntimeError error) noexcept;

enum class InputSendResult : std::uint8_t {
    Sent,
    NotReady,
    Backpressured,
    SpectatorOnly,
    InvalidMovement,
    InvalidAction,
    InvalidClientTick,
    SequenceExhausted,
    TransportFailure,
};

struct ReplicationStatistics {
    std::optional<std::chrono::milliseconds> latest_snapshot_age;
    float accepted_snapshots_per_second{};
    std::uint64_t accepted_snapshot_count{};
};

struct Settings {
    protocol::JoinRole requested_role{protocol::JoinRole::Player};
    bool input_redundancy{true};
    bool log_prediction_scope_changes{};
    bool log_prediction_frontier_changes{};
    bool log_prediction_reconciliation_details{};
};

inline constexpr std::size_t kInputPauseThreshold = 8;
inline constexpr std::size_t kInputResumeThreshold = 2;
inline constexpr std::size_t kMaximumInputPacketsPerFlush = 4;
inline constexpr auto kClientStatusInterval = std::chrono::milliseconds{200};

struct InputFlowStatistics {
    protocol::JoinRole requested_role{protocol::JoinRole::Player};
    std::optional<protocol::JoinRole> accepted_role;
    protocol::InputSequenceId acknowledged_through;
    protocol::InputSequenceId receive_through;
    protocol::InputSequenceId transmitted_through;
    std::size_t unsent_count{};
    std::size_t unsent_high_water_mark{};
    bool production_paused{};
    std::uint64_t pause_count{};
    std::chrono::milliseconds accumulated_paused_time{};
    std::uint64_t status_count{};
};

struct AcceptedSnapshot {
    protocol::FullSnapshot snapshot;
    std::chrono::steady_clock::time_point arrival_time;
};

struct PredictionIdentityRemap {
    protocol::PredictionKey prediction_key;
    protocol::EntityId previous_entity_id;
    protocol::EntityId current_entity_id;

    bool operator==(const PredictionIdentityRemap&) const = default;
};

enum class PredictionCorrectionSource : std::uint8_t {
    Local,
    Remote,
};

struct PredictionCorrection {
    std::uint64_t sequence{};
    protocol::EntityId entity_id;
    mycore::math::Vector2 pre_correction_position;
    mycore::math::Vector2 corrected_position;
    float mass{};
    float distance{};
    PredictionCorrectionSource source{PredictionCorrectionSource::Local};

    bool operator==(const PredictionCorrection&) const = default;
};

enum class DebugFaultKind : std::uint8_t {
    PositionDivergence,
    InputPacketLoss,
};

enum class DebugFaultPhase : std::uint8_t {
    Armed,
    Triggered,
    Completed,
};

struct DebugFaultReceipt {
    std::uint64_t id{};
    DebugFaultKind kind{DebugFaultKind::PositionDivergence};
    DebugFaultPhase phase{DebugFaultPhase::Armed};
    std::size_t requested_count{};
    std::size_t observed_count{};

    bool operator==(const DebugFaultReceipt&) const = default;
};

struct AuthorityReceiptRejectionStatistics {
    std::uint64_t invalid_retirement_count{};
    std::uint64_t sequence_gap_count{};
    std::uint64_t conflicting_receipt_count{};
    std::uint64_t duplicate_event_key_count{};
    std::uint64_t capacity_exceeded_count{};

    bool operator==(const AuthorityReceiptRejectionStatistics&) const = default;
};

struct ProcessEventsResult {
    std::optional<RuntimeError> error;
    std::vector<AcceptedSnapshot> accepted_snapshots;

    [[nodiscard]] bool has_value() const noexcept {
        return error.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.has_value();
    }
};

inline constexpr std::size_t kPredictionHistoryCapacity = 256;
inline constexpr std::size_t kPredictionEventBatchCapacity = 512;
inline constexpr std::size_t kDebugFaultReceiptCapacity = 32;
using PredictionEventBatch = mycore::rollback::EventBatch<prediction::WorldModel>;

struct PredictionStatistics {
    prediction::PredictionProfile requested_profile{
        prediction::PredictionProfile::InteractionClosure};
    prediction::PredictionProfile active_profile{prediction::PredictionProfile::InteractionClosure};
    prediction::PredictionFallbackReason fallback_reason{
        prediction::PredictionFallbackReason::None};
    prediction::MechanicMask mechanics{};
    prediction::StateDomainMask required_domains{};
    prediction::CausalChannelMask required_causal_channels{};
    bool input_redundancy_enabled{true};
    protocol::InputSequenceId last_input_sent;
    protocol::InputSequenceId last_input_acknowledged;
    protocol::InputSequenceId last_timeline_input_submitted;
    std::size_t unacknowledged_input_count{};
    std::size_t deferred_prediction_input_count{};
    std::size_t history_count{};
    std::size_t history_capacity{kPredictionHistoryCapacity};
    std::size_t history_high_water_mark{};
    std::uint64_t scope_epoch{};
    std::uint64_t scope_replay_horizon_ticks{};
    std::size_t scope_owner_count{};
    std::size_t scope_event_owner_count{};
    std::size_t scope_player_count{};
    std::size_t scope_food_count{};
    std::uint64_t scope_rebase_count{};
    std::uint8_t latest_server_pending_input_count{};
    std::uint8_t server_pending_input_high_water_mark{};
    protocol::SnapshotId rollback_snapshot_id;
    std::uint32_t rollback_server_tick{};
    std::uint64_t authoritative_tick{};
    std::uint64_t predicted_tick{};
    std::uint64_t prediction_lead_ticks{};
    protocol::InputSequenceId rollback_input_acknowledgement;
    protocol::InputSequenceId replay_first_input;
    protocol::InputSequenceId replay_last_input;
    std::uint16_t checkpoint_schema_id{};
    std::uint64_t replicated_checkpoint_digest{};
    std::uint64_t authoritative_prediction_digest{};
    std::uint64_t predicted_digest{};
    std::size_t checkpoint_storage_bytes{};
    std::size_t latest_replay_count{};
    std::uint64_t total_replayed_input_count{};
    std::size_t maximum_replay_count{};
    double latest_replay_milliseconds{};
    double average_replay_milliseconds{};
    double replay_p50_milliseconds{};
    double replay_p95_milliseconds{};
    double replay_p99_milliseconds{};
    double maximum_replay_milliseconds{};
    bool latest_rules_changed{};
    bool latest_allocator_changed{};
    bool latest_structural_change{};
    float latest_maximum_position_delta{};
    float latest_maximum_mass_delta{};
    std::size_t latest_owner_difference_count{};
    std::size_t latest_player_difference_count{};
    std::size_t latest_food_difference_count{};
    std::size_t latest_entity_creation_count{};
    std::size_t latest_entity_removal_count{};
    std::size_t pending_predicted_spawn_count{};
    std::uint64_t matched_predicted_spawn_count{};
    std::uint64_t rejected_predicted_spawn_count{};
    std::uint64_t authority_only_spawn_count{};
    std::uint64_t ambiguous_predicted_spawn_count{};
    std::size_t remote_assumption_count{};
    std::uint64_t remote_assumption_source_tick_min{};
    std::uint64_t remote_assumption_source_tick_max{};
    std::uint64_t remote_assumption_applied_tick_first{};
    std::uint64_t remote_assumption_applied_tick_last{};
    std::uint64_t reconciliation_count{};
    std::uint64_t nonzero_correction_count{};
    std::uint64_t remote_entity_correction_count{};
    std::size_t latest_remote_entity_correction_count{};
    float latest_correction_distance{};
    float maximum_correction_distance{};
    float latest_remote_correction_distance{};
    float maximum_remote_correction_distance{};
    float corrections_per_minute{};
    mycore::math::Vector2 accumulated_correction_displacement;
    std::uint64_t correction_sequence_since_hard_resync{};
    std::uint64_t replay_over_budget_count{};
    std::uint64_t hard_resync_count{};
    std::uint64_t acknowledgement_catch_up_count{};
    std::size_t pending_injected_input_drop_count{};
    std::uint64_t injected_input_drop_count{};
    std::uint64_t injected_prediction_error_count{};
    AuthorityReceiptRejectionStatistics authority_receipt_rejections;
    std::uint64_t prediction_event_queue_overflow_count{};
    protocol::AuthorityReceiptSequenceId authority_receipts_accepted_through;
    protocol::AuthorityReceiptSequenceId authority_receipts_published_through;
    protocol::AuthorityReceiptSequenceId authority_receipts_server_retired_through;
    std::size_t authority_receipt_retained_count{};
    std::size_t authority_receipt_pending_publication_count{};
    std::size_t pending_prediction_event_batch_count{};
};

class Runtime {
public:
    explicit Runtime(mycore::net_transport::Endpoint& endpoint, Settings settings = {});
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;

    [[nodiscard]] ProcessEventsResult
    process_events(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    [[nodiscard]] InputSendResult submit_input(std::uint32_t client_tick,
                                               mycore::math::Vector2 movement,
                                               std::uint16_t action_bits = 0);
    // Compatibility spelling for callers written before local submission could be buffered.
    [[nodiscard]] InputSendResult send_input(std::uint32_t client_tick,
                                             mycore::math::Vector2 movement,
                                             std::uint16_t action_bits = 0);
    [[nodiscard]] bool disconnect();
    [[nodiscard]] std::vector<PredictionEventBatch> take_prediction_event_batches();

    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] const replication::ReplicatedWorld& world() const& noexcept;
    [[nodiscard]] const replication::ReplicatedWorld& world() const&& = delete;
    [[nodiscard]] protocol::ClientId client_id() const noexcept;
    [[nodiscard]] std::optional<protocol::JoinRole> accepted_role() const noexcept;
    [[nodiscard]] bool input_production_paused() const noexcept;
    [[nodiscard]] protocol::EntityId controlled_entity_id() const noexcept;
    [[nodiscard]] protocol::SessionMode session_mode() const noexcept;
    [[nodiscard]] std::span<const protocol::EntityId> owned_entity_ids() const& noexcept;
    [[nodiscard]] std::span<const protocol::EntityId> owned_entity_ids() const&& = delete;
    [[nodiscard]] protocol::EntityId primary_entity_id() const noexcept;
    [[nodiscard]] protocol::EntityId follow_entity_id() const noexcept;
    [[nodiscard]] std::optional<protocol::WorldRules> world_rules() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> defeat_tick() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> respawn_available_tick() const noexcept;
    [[nodiscard]] std::uint32_t respawn_cooldown_ticks() const noexcept;
    [[nodiscard]] std::optional<protocol::PlayerAbsorbed> latest_absorption() const noexcept;
    [[nodiscard]] protocol::InputSequenceId latest_respawn_request_id() const noexcept;
    [[nodiscard]] protocol::RespawnResult latest_respawn_result() const noexcept;
    [[nodiscard]] mycore::net_transport::ConnectionHandle connection_handle() const noexcept;
    [[nodiscard]] const simulation::World* predicted_world() const& noexcept;
    [[nodiscard]] const simulation::World* predicted_world() const&& = delete;
    [[nodiscard]] protocol::EntityId predicted_primary_entity_id() const noexcept;
    [[nodiscard]] std::span<const protocol::EntityId> predicted_owned_entity_ids() const& noexcept;
    [[nodiscard]] std::span<const protocol::EntityId> predicted_owned_entity_ids() const&& = delete;
    [[nodiscard]] std::span<const protocol::EntityId> predicted_scope_entity_ids() const& noexcept;
    [[nodiscard]] std::span<const protocol::EntityId> predicted_scope_entity_ids() const&& = delete;
    [[nodiscard]] std::span<const PredictionIdentityRemap>
    latest_prediction_identity_remaps() const& noexcept;
    [[nodiscard]] std::span<const PredictionIdentityRemap>
    latest_prediction_identity_remaps() const&& = delete;
    [[nodiscard]] std::span<const PredictionCorrection>
    recent_prediction_corrections() const& noexcept;
    [[nodiscard]] std::span<const PredictionCorrection>
    recent_prediction_corrections() const&& = delete;
    [[nodiscard]] std::optional<mycore::math::Vector2> predicted_position() const noexcept;
    [[nodiscard]] std::optional<mycore::math::Vector2> pre_correction_position() const noexcept;
    [[nodiscard]] std::span<const mycore::math::Vector2> latest_replay_path() const& noexcept;
    [[nodiscard]] std::span<const mycore::math::Vector2> latest_replay_path() const&& = delete;
    [[nodiscard]] std::span<const mycore::math::Vector2>
    latest_correction_replay_path() const& noexcept;
    [[nodiscard]] std::span<const mycore::math::Vector2>
    latest_correction_replay_path() const&& = delete;
    [[nodiscard]] bool debug_inject_prediction_error(mycore::math::Vector2 displacement);
    [[nodiscard]] bool debug_drop_next_input_packets(std::size_t count);
    [[nodiscard]] std::span<const DebugFaultReceipt> debug_fault_receipts() const& noexcept;
    [[nodiscard]] std::span<const DebugFaultReceipt> debug_fault_receipts() const&& = delete;
    [[nodiscard]] PredictionStatistics
    prediction_statistics(std::chrono::steady_clock::time_point now =
                              std::chrono::steady_clock::now()) const noexcept;
    [[nodiscard]] ReplicationStatistics
    replication_statistics(std::chrono::steady_clock::time_point now =
                               std::chrono::steady_clock::now()) const noexcept;
    [[nodiscard]] InputFlowStatistics
    input_flow_statistics(std::chrono::steady_clock::time_point now =
                              std::chrono::steady_clock::now()) const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dots::client_runtime

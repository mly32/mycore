#pragma once

#include "mycore/core/strong_id.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mycore::rollback {

struct CommandSequenceTag;
struct ScopeEpochTag;

using CommandSequence = core::StrongId<CommandSequenceTag, std::uint64_t>;
using ScopeEpoch = core::StrongId<ScopeEpochTag, std::uint64_t>;

struct HistorySettings {
    std::size_t capacity{64};
};

enum class CommitKind : std::uint8_t {
    Initialize,
    Advance,
    Reconcile,
    AuthorityRefresh,
    ScopeRebase,
    HardResync,
    AuthorityOnly,
};

enum class EventTransition : std::uint8_t {
    FirstPredicted,
    Revised,
    Retracted,
    Confirmed,
    AuthorityOnly,
};

enum class AuthorityEventDisposition : std::uint8_t {
    Confirmed,
    Rejected,
};

enum class ConsequencePolicy : std::uint8_t {
    PredictOnce,
    PredictCancelable,
    ConfirmOnce,
};

enum class TimelineErrorCode : std::uint8_t {
    NotInitialized,
    AlreadyInitialized,
    InvalidCommandSequence,
    NonMonotonicCommandSequence,
    HistoryExhausted,
    StaleAuthority,
    InvalidAcknowledgement,
    IncompatibleScope,
    IncompatibleAuthority,
    DuplicateEventKey,
    ConflictingAuthorityEvent,
    ModelRestoreFailed,
    StimulusRefreshFailed,
    ModelStepFailed,
};

enum class ConsequenceOperation : std::uint8_t {
    First,
    Predict,
    Revise,
    Cancel,
    Confirm,
    Confirmed,
};

struct TimelineStatistics {
    std::uint64_t initialization_count{};
    std::uint64_t advance_count{};
    std::uint64_t reconciliation_count{};
    std::uint64_t authority_refresh_count{};
    std::uint64_t scope_rebase_count{};
    std::uint64_t hard_resync_count{};
    std::uint64_t replayed_frame_count{};
    std::uint64_t failure_count{};
    std::size_t history_high_water{};
};

struct ConsequenceDispatchStatistics {
    std::uint64_t delivered_count{};
    std::uint64_t suppressed_count{};
    std::uint64_t revised_count{};
    std::uint64_t canceled_count{};
    std::uint64_t confirmed_count{};
    std::uint64_t failure_count{};
};

[[nodiscard]] std::string_view library_name() noexcept;
[[nodiscard]] std::string_view timeline_error_name(TimelineErrorCode error) noexcept;

} // namespace mycore::rollback

#include "mycore/rollback/types.hpp"

namespace mycore::rollback {

std::string_view library_name() noexcept {
    return "MyCore::Rollback";
}

std::string_view timeline_error_name(TimelineErrorCode error) noexcept {
    switch (error) {
    case TimelineErrorCode::NotInitialized:
        return "not_initialized";
    case TimelineErrorCode::AlreadyInitialized:
        return "already_initialized";
    case TimelineErrorCode::InvalidCommandSequence:
        return "invalid_command_sequence";
    case TimelineErrorCode::NonMonotonicCommandSequence:
        return "non_monotonic_command_sequence";
    case TimelineErrorCode::HistoryExhausted:
        return "history_exhausted";
    case TimelineErrorCode::StaleAuthority:
        return "stale_authority";
    case TimelineErrorCode::InvalidAcknowledgement:
        return "invalid_acknowledgement";
    case TimelineErrorCode::IncompatibleScope:
        return "incompatible_scope";
    case TimelineErrorCode::IncompatibleAuthority:
        return "incompatible_authority";
    case TimelineErrorCode::DuplicateEventKey:
        return "duplicate_event_key";
    case TimelineErrorCode::ConflictingAuthorityEvent:
        return "conflicting_authority_event";
    case TimelineErrorCode::ModelRestoreFailed:
        return "model_restore_failed";
    case TimelineErrorCode::ModelStepFailed:
        return "model_step_failed";
    }
    return "unknown";
}

} // namespace mycore::rollback

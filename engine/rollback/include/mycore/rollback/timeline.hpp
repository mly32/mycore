#pragma once

#include "mycore/rollback/types.hpp"
#include "mycore/time/time.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace mycore::rollback {

template <class Model>
concept RollbackModel =
    std::copy_constructible<Model> && std::movable<typename Model::State> &&
    std::copy_constructible<typename Model::Checkpoint> &&
    std::copy_constructible<typename Model::Stimulus> &&
    std::copy_constructible<typename Model::Scope> &&
    std::copy_constructible<typename Model::Event> &&
    std::copy_constructible<typename Model::EventKey> && std::movable<typename Model::StateDiff> &&
    std::copy_constructible<typename Model::StateDigest> && std::movable<typename Model::Error> &&
    requires(const Model& model,
             const typename Model::Checkpoint& checkpoint,
             const typename Model::Scope& scope,
             typename Model::State& state,
             const typename Model::State& const_state,
             const typename Model::Stimulus& stimulus,
             const typename Model::Event& event,
             const typename Model::EventKey& event_key,
             const typename Model::StateDigest& digest) {
        typename Model::State;
        typename Model::Checkpoint;
        typename Model::Stimulus;
        typename Model::Scope;
        typename Model::Event;
        typename Model::EventKey;
        typename Model::EventKeyHash;
        typename Model::StateDiff;
        typename Model::StateDigest;
        typename Model::Error;
        {
            model.restore(checkpoint, scope)
        } -> std::same_as<std::variant<typename Model::State, typename Model::Error>>;
        { model.capture(const_state, scope) } -> std::same_as<typename Model::Checkpoint>;
        {
            model.step(state, stimulus, scope)
        } -> std::same_as<std::variant<std::vector<typename Model::Event>, typename Model::Error>>;
        { model.digest(checkpoint, scope) } -> std::same_as<typename Model::StateDigest>;
        { model.diff(const_state, const_state, scope) } -> std::same_as<typename Model::StateDiff>;
        { model.event_key(event) } -> std::same_as<typename Model::EventKey>;
        { typename Model::EventKeyHash{}(event_key) } -> std::convertible_to<std::size_t>;
        { checkpoint == checkpoint } -> std::convertible_to<bool>;
        { event == event } -> std::convertible_to<bool>;
        { event_key == event_key } -> std::convertible_to<bool>;
        { digest == digest } -> std::convertible_to<bool>;
    };

template <class Refresh, class Model>
concept ReplayStimulusRefresh =
    RollbackModel<Model> && requires(Refresh& refresh,
                                     CommandSequence sequence,
                                     const typename Model::Stimulus& previous,
                                     const typename Model::State& state,
                                     const typename Model::Scope& scope) {
        {
            refresh(sequence, previous, state, scope)
        } -> std::same_as<std::variant<typename Model::Stimulus, typename Model::Error>>;
    };

template <RollbackModel Model> struct AuthorityEvent {
    AuthorityEventDisposition disposition{AuthorityEventDisposition::Confirmed};
    typename Model::EventKey key;
    std::optional<typename Model::Event> event;
};

template <RollbackModel Model> struct AuthorityFrame {
    time::Tick tick;
    std::optional<CommandSequence> acknowledged_through;
    ScopeEpoch scope_epoch;
    typename Model::Checkpoint checkpoint;
    std::vector<AuthorityEvent<Model>> events;
};

template <RollbackModel Model> struct FrameRecord {
    time::Tick tick;
    CommandSequence command_sequence;
    typename Model::Stimulus stimulus;
    typename Model::Checkpoint checkpoint;
    typename Model::StateDigest digest;
    std::vector<typename Model::Event> events;
    ScopeEpoch scope_epoch;
};

template <RollbackModel Model> struct EventChange {
    EventTransition transition{EventTransition::FirstPredicted};
    typename Model::EventKey key;
    std::optional<typename Model::Event> previous;
    std::optional<typename Model::Event> current;
};

template <RollbackModel Model> struct Commit {
    CommitKind kind{CommitKind::Advance};
    time::Tick previous_predicted_tick;
    time::Tick authoritative_tick;
    time::Tick predicted_tick;
    std::optional<CommandSequence> acknowledged_through;
    std::size_t replayed_frame_count{};
    typename Model::StateDiff state_diff;
    typename Model::StateDigest authoritative_digest;
    typename Model::StateDigest predicted_digest;
    std::optional<typename Model::StateDigest> prior_prediction_digest_at_authority;
    std::vector<EventChange<Model>> event_changes;
    std::vector<typename Model::EventKey> retired_event_keys;
};

template <RollbackModel Model> struct TimelineFailure {
    TimelineErrorCode code{TimelineErrorCode::NotInitialized};
    std::optional<typename Model::Error> model_error;
};

template <RollbackModel Model, class Value>
using TimelineResult = std::variant<Value, TimelineFailure<Model>>;

template <RollbackModel Model> using CommitResult = TimelineResult<Model, Commit<Model>>;

template <RollbackModel Model> using InitializeResult = CommitResult<Model>;

template <RollbackModel Model> class Timeline {
public:
    explicit Timeline(Model model, HistorySettings settings = {})
        : model_(std::move(model)),
          settings_(settings) {
        if (settings_.capacity == 0) {
            throw std::invalid_argument{"Rollback history capacity must be greater than zero"};
        }
    }

    [[nodiscard]] InitializeResult<Model> initialize(const AuthorityFrame<Model>& authority,
                                                     typename Model::Scope scope) {
        if (initialized()) {
            return failure(TimelineErrorCode::AlreadyInitialized);
        }
        if (!valid_epoch(authority.scope_epoch) ||
            !valid_optional_sequence(authority.acknowledged_through)) {
            return failure(TimelineErrorCode::IncompatibleAuthority);
        }
        if (const auto event_error = validate_authority_events(authority.events)) {
            return failure(*event_error);
        }

        auto restored = model_.restore(authority.checkpoint, scope);
        if (auto* error = std::get_if<typename Model::Error>(&restored)) {
            return failure(TimelineErrorCode::ModelRestoreFailed, std::move(*error));
        }
        auto scratch = std::move(std::get<typename Model::State>(restored));

        const EventIndex no_predicted_events;
        auto resolved =
            resolve_event_changes(no_predicted_events, no_predicted_events, authority.events);
        if (auto* error = std::get_if<TimelineErrorCode>(&resolved)) {
            return failure(*error);
        }
        auto event_resolution = std::move(std::get<EventResolution>(resolved));
        const auto digest = model_.digest(authority.checkpoint, scope);
        auto state_diff = model_.diff(scratch, scratch, scope);

        state_.emplace(std::move(scratch));
        base_checkpoint_.emplace(authority.checkpoint);
        scope_.emplace(std::move(scope));
        history_.clear();
        authoritative_tick_ = authority.tick;
        predicted_tick_ = authority.tick;
        scope_epoch_ = authority.scope_epoch;
        acknowledged_through_ = authority.acknowledged_through;
        last_submitted_sequence_ = authority.acknowledged_through;
        ++statistics_.initialization_count;
        return Commit<Model>{
            .kind = CommitKind::Initialize,
            .previous_predicted_tick = authoritative_tick_,
            .authoritative_tick = authoritative_tick_,
            .predicted_tick = predicted_tick_,
            .acknowledged_through = acknowledged_through_,
            .replayed_frame_count = 0,
            .state_diff = std::move(state_diff),
            .authoritative_digest = digest,
            .predicted_digest = digest,
            .prior_prediction_digest_at_authority = std::nullopt,
            .event_changes = std::move(event_resolution.changes),
            .retired_event_keys = std::move(event_resolution.retired_keys),
        };
    }

    [[nodiscard]] CommitResult<Model> advance(CommandSequence sequence,
                                              typename Model::Stimulus stimulus) {
        if (!state_ || !base_checkpoint_ || !scope_) {
            return failure(TimelineErrorCode::NotInitialized);
        }
        if (!sequence.is_valid()) {
            return failure(TimelineErrorCode::InvalidCommandSequence);
        }
        if ((last_submitted_sequence_ && sequence <= *last_submitted_sequence_) ||
            (acknowledged_through_ && sequence <= *acknowledged_through_)) {
            return failure(TimelineErrorCode::NonMonotonicCommandSequence);
        }
        if (history_.size() >= settings_.capacity) {
            return failure(TimelineErrorCode::HistoryExhausted);
        }

        const auto& active_scope = *scope_;
        auto restored = model_.restore(head_checkpoint(), active_scope);
        if (auto* error = std::get_if<typename Model::Error>(&restored)) {
            return failure(TimelineErrorCode::ModelRestoreFailed, std::move(*error));
        }
        auto scratch = std::move(std::get<typename Model::State>(restored));

        auto stepped = model_.step(scratch, stimulus, active_scope);
        if (auto* error = std::get_if<typename Model::Error>(&stepped)) {
            return failure(TimelineErrorCode::ModelStepFailed, std::move(*error));
        }
        auto events = std::move(std::get<std::vector<typename Model::Event>>(stepped));

        EventIndex existing_events;
        if (const auto error = build_event_map(history_, existing_events)) {
            return failure(*error);
        }
        if (const auto error = append_events(existing_events, events)) {
            return failure(*error);
        }

        const auto next_tick = predicted_tick_ + time::TickDelta{1};
        auto checkpoint = model_.capture(scratch, active_scope);
        auto digest = model_.digest(checkpoint, active_scope);
        auto state_diff = model_.diff(*state_, scratch, active_scope);
        const auto previous_tick = predicted_tick_;

        std::vector<EventChange<Model>> event_changes;
        event_changes.reserve(events.size());
        for (const auto& event : events) {
            event_changes.push_back(EventChange<Model>{
                .transition = EventTransition::FirstPredicted,
                .key = model_.event_key(event),
                .previous = std::nullopt,
                .current = event,
            });
        }

        history_.push_back(FrameRecord<Model>{
            .tick = next_tick,
            .command_sequence = sequence,
            .stimulus = std::move(stimulus),
            .checkpoint = std::move(checkpoint),
            .digest = digest,
            .events = std::move(events),
            .scope_epoch = scope_epoch_,
        });
        state_.emplace(std::move(scratch));
        predicted_tick_ = next_tick;
        last_submitted_sequence_ = sequence;
        ++statistics_.advance_count;
        statistics_.history_high_water = std::max(statistics_.history_high_water, history_.size());

        return Commit<Model>{
            .kind = CommitKind::Advance,
            .previous_predicted_tick = previous_tick,
            .authoritative_tick = authoritative_tick_,
            .predicted_tick = predicted_tick_,
            .acknowledged_through = acknowledged_through_,
            .replayed_frame_count = 0,
            .state_diff = std::move(state_diff),
            .authoritative_digest = model_.digest(*base_checkpoint_, active_scope),
            .predicted_digest = digest,
            .prior_prediction_digest_at_authority = std::nullopt,
            .event_changes = std::move(event_changes),
            .retired_event_keys = {},
        };
    }

    [[nodiscard]] CommitResult<Model> reconcile(const AuthorityFrame<Model>& authority) {
        if (!state_ || !base_checkpoint_ || !scope_) {
            return failure(TimelineErrorCode::NotInitialized);
        }
        if (authority.scope_epoch != scope_epoch_) {
            return failure(TimelineErrorCode::IncompatibleScope);
        }
        const auto retain_stimulus = [](CommandSequence,
                                        const typename Model::Stimulus& stimulus,
                                        const typename Model::State&,
                                        const typename Model::Scope&)
            -> std::variant<typename Model::Stimulus, typename Model::Error> {
            return stimulus;
        };
        return reconcile_with_scope(authority,
                                    *scope_,
                                    CommitKind::Reconcile,
                                    AuthorityTickRule::StrictlyNewer,
                                    retain_stimulus);
    }

    template <class Refresh>
        requires ReplayStimulusRefresh<Refresh, Model>
    [[nodiscard]] CommitResult<Model>
    reconcile_with_stimulus_refresh(const AuthorityFrame<Model>& authority, Refresh refresh) {
        if (!state_ || !base_checkpoint_ || !scope_) {
            return failure(TimelineErrorCode::NotInitialized);
        }
        if (authority.scope_epoch != scope_epoch_) {
            return failure(TimelineErrorCode::IncompatibleScope);
        }
        return reconcile_with_scope(authority,
                                    *scope_,
                                    CommitKind::Reconcile,
                                    AuthorityTickRule::StrictlyNewer,
                                    std::move(refresh));
    }

    [[nodiscard]] CommitResult<Model> rebase_scope(const AuthorityFrame<Model>& authority,
                                                   typename Model::Scope scope) {
        if (!state_ || !base_checkpoint_ || !scope_) {
            return failure(TimelineErrorCode::NotInitialized);
        }
        if (!authority.scope_epoch.is_valid() || authority.scope_epoch <= scope_epoch_) {
            return failure(TimelineErrorCode::IncompatibleScope);
        }
        const auto retain_stimulus = [](CommandSequence,
                                        const typename Model::Stimulus& stimulus,
                                        const typename Model::State&,
                                        const typename Model::Scope&)
            -> std::variant<typename Model::Stimulus, typename Model::Error> {
            return stimulus;
        };
        return reconcile_with_scope(authority,
                                    std::move(scope),
                                    CommitKind::ScopeRebase,
                                    AuthorityTickRule::SameOrNewer,
                                    retain_stimulus);
    }

    template <class Refresh>
        requires ReplayStimulusRefresh<Refresh, Model>
    [[nodiscard]] CommitResult<Model> rebase_scope_with_stimulus_refresh(
        const AuthorityFrame<Model>& authority, typename Model::Scope scope, Refresh refresh) {
        if (!state_ || !base_checkpoint_ || !scope_) {
            return failure(TimelineErrorCode::NotInitialized);
        }
        if (!authority.scope_epoch.is_valid() || authority.scope_epoch <= scope_epoch_) {
            return failure(TimelineErrorCode::IncompatibleScope);
        }
        return reconcile_with_scope(authority,
                                    std::move(scope),
                                    CommitKind::ScopeRebase,
                                    AuthorityTickRule::SameOrNewer,
                                    std::move(refresh));
    }

    [[nodiscard]] CommitResult<Model> hard_resync(const AuthorityFrame<Model>& authority,
                                                  typename Model::Scope scope) {
        if (!state_ || !base_checkpoint_ || !scope_) {
            return failure(TimelineErrorCode::NotInitialized);
        }
        if (authority.scope_epoch < scope_epoch_) {
            return failure(TimelineErrorCode::IncompatibleScope);
        }
        if (const auto error = validate_authority(
                authority, AuthorityTickRule::SameOrNewer, SameTickCheckpointRule::MayReplace)) {
            return failure(*error);
        }

        auto restored = model_.restore(authority.checkpoint, scope);
        if (auto* error = std::get_if<typename Model::Error>(&restored)) {
            return failure(TimelineErrorCode::ModelRestoreFailed, std::move(*error));
        }
        auto scratch = std::move(std::get<typename Model::State>(restored));

        EventIndex old_events;
        if (const auto error = build_event_map(history_, old_events)) {
            return failure(*error);
        }
        const EventIndex no_predicted_events;
        auto resolved = resolve_event_changes(old_events, no_predicted_events, authority.events);
        if (auto* error = std::get_if<TimelineErrorCode>(&resolved)) {
            return failure(*error);
        }
        auto event_resolution = std::move(std::get<EventResolution>(resolved));

        auto state_diff = model_.diff(*state_, scratch, scope);
        const auto previous_tick = predicted_tick_;
        const auto authority_digest = model_.digest(authority.checkpoint, scope);
        const auto previous_digest = prior_digest_for_authority(authority.tick);

        state_.emplace(std::move(scratch));
        base_checkpoint_.emplace(authority.checkpoint);
        scope_.emplace(std::move(scope));
        history_.clear();
        authoritative_tick_ = authority.tick;
        predicted_tick_ = authority.tick;
        scope_epoch_ = authority.scope_epoch;
        acknowledged_through_ = authority.acknowledged_through;
        if (authority.acknowledged_through &&
            (!last_submitted_sequence_ ||
             *last_submitted_sequence_ < *authority.acknowledged_through)) {
            last_submitted_sequence_ = authority.acknowledged_through;
        }
        ++statistics_.hard_resync_count;

        return Commit<Model>{
            .kind = CommitKind::HardResync,
            .previous_predicted_tick = previous_tick,
            .authoritative_tick = authoritative_tick_,
            .predicted_tick = predicted_tick_,
            .acknowledged_through = acknowledged_through_,
            .replayed_frame_count = 0,
            .state_diff = std::move(state_diff),
            .authoritative_digest = authority_digest,
            .predicted_digest = authority_digest,
            .prior_prediction_digest_at_authority = previous_digest,
            .event_changes = std::move(event_resolution.changes),
            .retired_event_keys = std::move(event_resolution.retired_keys),
        };
    }

    [[nodiscard]] bool initialized() const noexcept {
        return state_.has_value() && base_checkpoint_.has_value() && scope_.has_value();
    }

    [[nodiscard]] const typename Model::State* state() const noexcept {
        return state_ ? &*state_ : nullptr;
    }

    [[nodiscard]] const typename Model::Scope* scope() const noexcept {
        return scope_ ? &*scope_ : nullptr;
    }

    [[nodiscard]] const std::deque<FrameRecord<Model>>& history() const noexcept {
        return history_;
    }

    [[nodiscard]] time::Tick authoritative_tick() const noexcept {
        return authoritative_tick_;
    }

    [[nodiscard]] time::Tick predicted_tick() const noexcept {
        return predicted_tick_;
    }

    [[nodiscard]] ScopeEpoch scope_epoch() const noexcept {
        return scope_epoch_;
    }

    [[nodiscard]] std::optional<CommandSequence> acknowledged_through() const noexcept {
        return acknowledged_through_;
    }

    [[nodiscard]] const TimelineStatistics& statistics() const noexcept {
        return statistics_;
    }

private:
    enum class AuthorityTickRule : std::uint8_t {
        StrictlyNewer,
        SameOrNewer,
    };

    enum class SameTickCheckpointRule : std::uint8_t {
        MustMatch,
        MayReplace,
    };

    struct EventIndex {
        std::unordered_map<typename Model::EventKey,
                           const typename Model::Event*,
                           typename Model::EventKeyHash>
            by_key;
        std::vector<typename Model::EventKey> journal_order;
    };

    struct EventResolution {
        std::vector<EventChange<Model>> changes;
        std::vector<typename Model::EventKey> retired_keys;
    };

    [[nodiscard]] TimelineFailure<Model>
    failure(TimelineErrorCode code,
            std::optional<typename Model::Error> model_error = std::nullopt) {
        ++statistics_.failure_count;
        return TimelineFailure<Model>{.code = code, .model_error = std::move(model_error)};
    }

    [[nodiscard]] static bool valid_epoch(ScopeEpoch epoch) noexcept {
        return epoch.is_valid();
    }

    [[nodiscard]] static bool
    valid_optional_sequence(std::optional<CommandSequence> sequence) noexcept {
        return !sequence || sequence->is_valid();
    }

    [[nodiscard]] const typename Model::Checkpoint& head_checkpoint() const {
        if (!history_.empty()) {
            return history_.back().checkpoint;
        }
        if (base_checkpoint_) {
            return *base_checkpoint_;
        }
        throw std::logic_error{"Rollback timeline has no committed checkpoint"};
    }

    [[nodiscard]] std::optional<TimelineErrorCode>
    validate_authority_events(const std::vector<AuthorityEvent<Model>>& events) const {
        std::unordered_map<typename Model::EventKey,
                           const AuthorityEvent<Model>*,
                           typename Model::EventKeyHash>
            keys;
        for (const auto& authority_event : events) {
            if (authority_event.disposition == AuthorityEventDisposition::Confirmed) {
                if (!authority_event.event ||
                    model_.event_key(*authority_event.event) != authority_event.key) {
                    return TimelineErrorCode::ConflictingAuthorityEvent;
                }
            } else if (authority_event.event) {
                return TimelineErrorCode::ConflictingAuthorityEvent;
            }

            const auto [found, inserted] = keys.emplace(authority_event.key, &authority_event);
            if (!inserted) {
                const auto& previous = *found->second;
                if (previous.disposition != authority_event.disposition ||
                    previous.event != authority_event.event) {
                    return TimelineErrorCode::ConflictingAuthorityEvent;
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<TimelineErrorCode> validate_authority(
        const AuthorityFrame<Model>& authority,
        AuthorityTickRule tick_rule,
        SameTickCheckpointRule checkpoint_rule = SameTickCheckpointRule::MustMatch) const {
        if (!base_checkpoint_) {
            return TimelineErrorCode::NotInitialized;
        }
        if (!valid_epoch(authority.scope_epoch) ||
            !valid_optional_sequence(authority.acknowledged_through)) {
            return TimelineErrorCode::IncompatibleAuthority;
        }
        if (tick_rule == AuthorityTickRule::StrictlyNewer) {
            if (authority.tick <= authoritative_tick_) {
                return TimelineErrorCode::StaleAuthority;
            }
        } else if (authority.tick < authoritative_tick_) {
            return TimelineErrorCode::StaleAuthority;
        }
        if (acknowledged_through_ && (!authority.acknowledged_through ||
                                      *authority.acknowledged_through < *acknowledged_through_)) {
            return TimelineErrorCode::InvalidAcknowledgement;
        }
        if (authority.acknowledged_through &&
            (!last_submitted_sequence_ ||
             *authority.acknowledged_through > *last_submitted_sequence_)) {
            return TimelineErrorCode::InvalidAcknowledgement;
        }
        if (checkpoint_rule == SameTickCheckpointRule::MustMatch &&
            tick_rule == AuthorityTickRule::SameOrNewer && authority.tick == authoritative_tick_ &&
            authority.checkpoint != *base_checkpoint_) {
            return TimelineErrorCode::IncompatibleAuthority;
        }
        return validate_authority_events(authority.events);
    }

    [[nodiscard]] std::optional<TimelineErrorCode>
    build_event_map(const std::deque<FrameRecord<Model>>& frames, EventIndex& result) const {
        for (const auto& frame : frames) {
            if (const auto error = append_events(result, frame.events)) {
                return error;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<TimelineErrorCode>
    append_events(EventIndex& result, const std::vector<typename Model::Event>& events) const {
        for (const auto& event : events) {
            const auto key = model_.event_key(event);
            if (!result.by_key.emplace(key, &event).second) {
                return TimelineErrorCode::DuplicateEventKey;
            }
            result.journal_order.push_back(key);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::variant<EventResolution, TimelineErrorCode>
    resolve_event_changes(const EventIndex& old_events,
                          const EventIndex& new_events,
                          const std::vector<AuthorityEvent<Model>>& authority_events) const {
        EventResolution result;
        result.changes.reserve(old_events.by_key.size() + new_events.by_key.size() +
                               authority_events.size());
        result.retired_keys.reserve(old_events.by_key.size() + authority_events.size());

        std::unordered_map<typename Model::EventKey,
                           const AuthorityEvent<Model>*,
                           typename Model::EventKeyHash>
            authority_by_key;
        for (const auto& event : authority_events) {
            authority_by_key.emplace(event.key, &event);
        }

        for (const auto& key : old_events.journal_order) {
            const auto* old_event = old_events.by_key.find(key)->second;
            if (const auto authority = authority_by_key.find(key);
                authority != authority_by_key.end()) {
                const auto& outcome = *authority->second;
                if (outcome.disposition == AuthorityEventDisposition::Confirmed) {
                    if (new_events.by_key.contains(key)) {
                        return TimelineErrorCode::ConflictingAuthorityEvent;
                    }
                    result.changes.push_back(EventChange<Model>{
                        .transition = EventTransition::Confirmed,
                        .key = key,
                        .previous = *old_event,
                        .current = outcome.event,
                    });
                } else {
                    if (new_events.by_key.contains(key)) {
                        return TimelineErrorCode::ConflictingAuthorityEvent;
                    }
                    result.changes.push_back(EventChange<Model>{
                        .transition = EventTransition::Retracted,
                        .key = key,
                        .previous = *old_event,
                        .current = std::nullopt,
                    });
                }
                result.retired_keys.push_back(key);
                continue;
            }

            if (const auto current = new_events.by_key.find(key);
                current != new_events.by_key.end()) {
                if (*old_event != *current->second) {
                    result.changes.push_back(EventChange<Model>{
                        .transition = EventTransition::Revised,
                        .key = key,
                        .previous = *old_event,
                        .current = *current->second,
                    });
                }
            } else {
                result.changes.push_back(EventChange<Model>{
                    .transition = EventTransition::Retracted,
                    .key = key,
                    .previous = *old_event,
                    .current = std::nullopt,
                });
                result.retired_keys.push_back(key);
            }
        }

        for (const auto& key : new_events.journal_order) {
            if (old_events.by_key.contains(key)) {
                continue;
            }
            if (const auto authority = authority_by_key.find(key);
                authority != authority_by_key.end()) {
                return TimelineErrorCode::ConflictingAuthorityEvent;
            }
            const auto* new_event = new_events.by_key.find(key)->second;
            result.changes.push_back(EventChange<Model>{
                .transition = EventTransition::FirstPredicted,
                .key = key,
                .previous = std::nullopt,
                .current = *new_event,
            });
        }

        for (const auto& authority_event : authority_events) {
            if (authority_by_key.find(authority_event.key)->second != &authority_event) {
                continue;
            }
            if (old_events.by_key.contains(authority_event.key)) {
                continue;
            }
            if (authority_event.disposition == AuthorityEventDisposition::Rejected) {
                continue;
            }
            if (new_events.by_key.contains(authority_event.key)) {
                return TimelineErrorCode::ConflictingAuthorityEvent;
            }
            result.changes.push_back(EventChange<Model>{
                .transition = EventTransition::AuthorityOnly,
                .key = authority_event.key,
                .previous = std::nullopt,
                .current = authority_event.event,
            });
            result.retired_keys.push_back(authority_event.key);
        }

        return result;
    }

    [[nodiscard]] std::optional<typename Model::StateDigest>
    prior_digest_for_authority(time::Tick tick) const {
        const auto frame =
            std::find_if(history_.begin(), history_.end(), [tick](const auto& record) {
                return record.tick == tick;
            });
        if (frame == history_.end()) {
            return std::nullopt;
        }
        return frame->digest;
    }

    template <class Refresh>
        requires ReplayStimulusRefresh<Refresh, Model>
    [[nodiscard]] CommitResult<Model> reconcile_with_scope(const AuthorityFrame<Model>& authority,
                                                           typename Model::Scope scope,
                                                           CommitKind kind,
                                                           AuthorityTickRule tick_rule,
                                                           Refresh refresh) {
        if (!state_ || !base_checkpoint_) {
            return failure(TimelineErrorCode::NotInitialized);
        }
        if (const auto error = validate_authority(authority, tick_rule)) {
            return failure(*error);
        }

        auto restored = model_.restore(authority.checkpoint, scope);
        if (auto* error = std::get_if<typename Model::Error>(&restored)) {
            return failure(TimelineErrorCode::ModelRestoreFailed, std::move(*error));
        }
        auto scratch = std::move(std::get<typename Model::State>(restored));

        EventIndex old_events;
        if (const auto error = build_event_map(history_, old_events)) {
            return failure(*error);
        }

        std::deque<FrameRecord<Model>> replayed_history;
        EventIndex replayed_events;
        auto replay_tick = authority.tick;
        for (const auto& frame : history_) {
            if (authority.acknowledged_through &&
                frame.command_sequence <= *authority.acknowledged_through) {
                continue;
            }

            auto refreshed =
                refresh(frame.command_sequence, frame.stimulus, std::as_const(scratch), scope);
            if (auto* error = std::get_if<typename Model::Error>(&refreshed)) {
                return failure(TimelineErrorCode::StimulusRefreshFailed, std::move(*error));
            }
            auto replay_stimulus = std::move(std::get<typename Model::Stimulus>(refreshed));
            auto stepped = model_.step(scratch, replay_stimulus, scope);
            if (auto* error = std::get_if<typename Model::Error>(&stepped)) {
                return failure(TimelineErrorCode::ModelStepFailed, std::move(*error));
            }
            auto events = std::move(std::get<std::vector<typename Model::Event>>(stepped));
            if (const auto error = append_events(replayed_events, events)) {
                return failure(*error);
            }

            replay_tick += time::TickDelta{1};
            auto checkpoint = model_.capture(scratch, scope);
            auto digest = model_.digest(checkpoint, scope);
            replayed_history.push_back(FrameRecord<Model>{
                .tick = replay_tick,
                .command_sequence = frame.command_sequence,
                .stimulus = std::move(replay_stimulus),
                .checkpoint = std::move(checkpoint),
                .digest = digest,
                .events = std::move(events),
                .scope_epoch = authority.scope_epoch,
            });
        }

        auto resolved = resolve_event_changes(old_events, replayed_events, authority.events);
        if (auto* error = std::get_if<TimelineErrorCode>(&resolved)) {
            return failure(*error);
        }
        auto event_resolution = std::move(std::get<EventResolution>(resolved));

        const auto previous_tick = predicted_tick_;
        const auto previous_digest = prior_digest_for_authority(authority.tick);
        const auto authority_digest = model_.digest(authority.checkpoint, scope);
        const auto predicted_digest =
            replayed_history.empty() ? authority_digest : replayed_history.back().digest;
        auto state_diff = model_.diff(*state_, scratch, scope);
        const auto replayed_count = replayed_history.size();

        state_.emplace(std::move(scratch));
        base_checkpoint_.emplace(authority.checkpoint);
        scope_.emplace(std::move(scope));
        history_ = std::move(replayed_history);
        authoritative_tick_ = authority.tick;
        predicted_tick_ = replay_tick;
        scope_epoch_ = authority.scope_epoch;
        acknowledged_through_ = authority.acknowledged_through;

        if (kind == CommitKind::Reconcile) {
            ++statistics_.reconciliation_count;
        } else {
            ++statistics_.scope_rebase_count;
        }
        statistics_.replayed_frame_count += replayed_count;
        statistics_.history_high_water = std::max(statistics_.history_high_water, history_.size());

        return Commit<Model>{
            .kind = kind,
            .previous_predicted_tick = previous_tick,
            .authoritative_tick = authoritative_tick_,
            .predicted_tick = predicted_tick_,
            .acknowledged_through = acknowledged_through_,
            .replayed_frame_count = replayed_count,
            .state_diff = std::move(state_diff),
            .authoritative_digest = authority_digest,
            .predicted_digest = predicted_digest,
            .prior_prediction_digest_at_authority = previous_digest,
            .event_changes = std::move(event_resolution.changes),
            .retired_event_keys = std::move(event_resolution.retired_keys),
        };
    }

    Model model_;
    HistorySettings settings_;
    std::optional<typename Model::State> state_;
    std::optional<typename Model::Checkpoint> base_checkpoint_;
    std::optional<typename Model::Scope> scope_;
    std::deque<FrameRecord<Model>> history_;
    time::Tick authoritative_tick_;
    time::Tick predicted_tick_;
    ScopeEpoch scope_epoch_;
    std::optional<CommandSequence> acknowledged_through_;
    std::optional<CommandSequence> last_submitted_sequence_;
    TimelineStatistics statistics_;
};

} // namespace mycore::rollback

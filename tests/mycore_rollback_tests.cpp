#include "mycore/rollback/rollback.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

struct ToyEventKey {
    std::uint32_t value{};

    auto operator<=>(const ToyEventKey&) const = default;
};

struct PulseEvent {
    ToyEventKey key;
    int payload{};

    auto operator<=>(const PulseEvent&) const = default;
};

struct NoticeEvent {
    ToyEventKey key;
    int payload{};

    auto operator<=>(const NoticeEvent&) const = default;
};

using ToyEvent = std::variant<PulseEvent, NoticeEvent>;

struct ToyEventKeyHash {
    [[nodiscard]] std::size_t operator()(ToyEventKey key) const noexcept {
        return key.value;
    }
};

struct ToyScope {
    int multiplier{1};

    auto operator<=>(const ToyScope&) const = default;
};

struct ToyState {
    mycore::time::Tick tick;
    int value{};
};

struct ToyCheckpoint {
    mycore::time::Tick tick;
    int value{};
    bool valid{true};

    auto operator<=>(const ToyCheckpoint&) const = default;
};

struct ToyStimulus {
    int delta{};
    int derived_delta{};
    std::optional<ToyEventKey> pulse_key;
    std::optional<ToyEventKey> notice_key;
    int fail_at_or_above{std::numeric_limits<int>::max()};
};

struct ToyStateDiff {
    int value_delta{};
};

enum class ToyError : std::uint8_t {
    InvalidCheckpoint,
    StepRejected,
};

struct ToyModel {
    using State = ToyState;
    using Checkpoint = ToyCheckpoint;
    using Stimulus = ToyStimulus;
    using Scope = ToyScope;
    using Event = ToyEvent;
    using EventKey = ToyEventKey;
    using EventKeyHash = ToyEventKeyHash;
    using StateDiff = ToyStateDiff;
    using StateDigest = std::uint64_t;
    using Error = ToyError;

    [[nodiscard]] std::variant<State, Error> restore(const Checkpoint& checkpoint,
                                                     const Scope& /*scope*/) const {
        if (!checkpoint.valid) {
            return Error::InvalidCheckpoint;
        }
        return State{.tick = checkpoint.tick, .value = checkpoint.value};
    }

    [[nodiscard]] Checkpoint capture(const State& state, const Scope& /*scope*/) const {
        return Checkpoint{.tick = state.tick, .value = state.value};
    }

    [[nodiscard]] std::variant<std::vector<Event>, Error>
    step(State& state, const Stimulus& stimulus, const Scope& scope) const {
        if (state.value >= stimulus.fail_at_or_above) {
            return Error::StepRejected;
        }

        state.value += (stimulus.delta + stimulus.derived_delta) * scope.multiplier;
        state.tick += mycore::time::TickDelta{1};

        std::vector<Event> events;
        if (stimulus.pulse_key) {
            events.emplace_back(PulseEvent{.key = *stimulus.pulse_key, .payload = state.value});
        }
        if (stimulus.notice_key) {
            events.emplace_back(NoticeEvent{.key = *stimulus.notice_key, .payload = state.value});
        }
        return events;
    }

    [[nodiscard]] StateDigest digest(const Checkpoint& checkpoint, const Scope& /*scope*/) const {
        return (checkpoint.tick.value() << 32U) ^ static_cast<std::uint32_t>(checkpoint.value);
    }

    [[nodiscard]] StateDiff
    diff(const State& previous, const State& current, const Scope& /*scope*/) const {
        return StateDiff{.value_delta = current.value - previous.value};
    }

    [[nodiscard]] EventKey event_key(const Event& event) const {
        return std::visit(
            [](const auto& value) {
                return value.key;
            },
            event);
    }
};

using Timeline = mycore::rollback::Timeline<ToyModel>;
using AuthorityFrame = mycore::rollback::AuthorityFrame<ToyModel>;
using AuthorityEvent = mycore::rollback::AuthorityEvent<ToyModel>;
using Commit = mycore::rollback::Commit<ToyModel>;
using Failure = mycore::rollback::TimelineFailure<ToyModel>;

[[nodiscard]] mycore::rollback::CommandSequence sequence(std::uint64_t value) {
    return mycore::rollback::CommandSequence{value};
}

[[nodiscard]] mycore::rollback::ScopeEpoch epoch(std::uint64_t value) {
    return mycore::rollback::ScopeEpoch{value};
}

[[nodiscard]] AuthorityFrame
authority(std::uint64_t tick,
          int value,
          std::optional<mycore::rollback::CommandSequence> acknowledgement = std::nullopt,
          mycore::rollback::ScopeEpoch scope_epoch = epoch(1),
          std::vector<AuthorityEvent> events = {}) {
    return AuthorityFrame{
        .tick = mycore::time::Tick{tick},
        .acknowledged_through = acknowledgement,
        .scope_epoch = scope_epoch,
        .checkpoint =
            ToyCheckpoint{.tick = mycore::time::Tick{tick}, .value = value, .valid = true},
        .events = std::move(events),
    };
}

[[nodiscard]] AuthorityEvent confirmed(ToyEvent event) {
    const auto key = std::visit(
        [](const auto& value) {
            return value.key;
        },
        event);
    return AuthorityEvent{
        .disposition = mycore::rollback::AuthorityEventDisposition::Confirmed,
        .key = key,
        .event = event,
    };
}

[[nodiscard]] AuthorityEvent rejected(ToyEventKey key) {
    return AuthorityEvent{
        .disposition = mycore::rollback::AuthorityEventDisposition::Rejected,
        .key = key,
        .event = std::nullopt,
    };
}

[[nodiscard]] Commit require_commit(const mycore::rollback::CommitResult<ToyModel>& result) {
    REQUIRE(std::holds_alternative<Commit>(result));
    return std::get<Commit>(result);
}

[[nodiscard]] Failure require_failure(const mycore::rollback::CommitResult<ToyModel>& result) {
    REQUIRE(std::holds_alternative<Failure>(result));
    return std::get<Failure>(result);
}

struct HandlerLog {
    std::vector<std::pair<ToyEventKey, int>> first;
    std::vector<std::pair<ToyEventKey, int>> predicted;
    std::vector<std::pair<ToyEventKey, int>> revised;
    std::vector<std::pair<ToyEventKey, int>> canceled;
    std::vector<std::pair<ToyEventKey, int>> confirmed;
};

struct PredictOncePulseHandler {
    using Event = PulseEvent;
    static constexpr auto policy = mycore::rollback::ConsequencePolicy::PredictOnce;

    HandlerLog* log{};
    bool fail{};

    [[nodiscard]] bool on_first(ToyEventKey key, const Event& event) {
        log->first.emplace_back(key, event.payload);
        return !fail;
    }
};

struct CancelablePulseHandler {
    using Event = PulseEvent;
    using Token = std::uint32_t;
    static constexpr auto policy = mycore::rollback::ConsequencePolicy::PredictCancelable;

    HandlerLog* log{};

    [[nodiscard]] std::optional<Token> on_predict(ToyEventKey key, const Event& event) {
        log->predicted.emplace_back(key, event.payload);
        return key.value;
    }

    [[nodiscard]] bool on_revise(Token& /*token*/, ToyEventKey key, const Event& event) {
        log->revised.emplace_back(key, event.payload);
        return true;
    }

    [[nodiscard]] bool on_cancel(Token& /*token*/, ToyEventKey key, const Event& event) {
        log->canceled.emplace_back(key, event.payload);
        return true;
    }

    [[nodiscard]] bool on_confirm(Token& /*token*/, ToyEventKey key, const Event& event) {
        log->confirmed.emplace_back(key, event.payload);
        return true;
    }
};

struct ConfirmOncePulseHandler {
    using Event = PulseEvent;
    static constexpr auto policy = mycore::rollback::ConsequencePolicy::ConfirmOnce;

    HandlerLog* log{};

    [[nodiscard]] bool on_confirmed(ToyEventKey key, const Event& event) {
        log->confirmed.emplace_back(key, event.payload);
        return true;
    }
};

} // namespace

TEST_CASE("Rollback timeline initializes and advances atomically", "[rollback][timeline]") {
    REQUIRE(mycore::rollback::library_name() == "MyCore::Rollback");
    REQUIRE_THROWS_AS((Timeline{ToyModel{}, {.capacity = 0}}), std::invalid_argument);

    Timeline timeline{ToyModel{}, {.capacity = 2}};
    const auto initialized = timeline.initialize(
        authority(10,
                  100,
                  std::nullopt,
                  epoch(1),
                  {confirmed(ToyEvent{PulseEvent{.key = ToyEventKey{5}, .payload = 100}})}),
        ToyScope{});
    const auto& initialization = require_commit(initialized);
    REQUIRE(initialization.kind == mycore::rollback::CommitKind::Initialize);
    REQUIRE(initialization.state_diff.value_delta == 0);
    REQUIRE(initialization.event_changes.size() == 1);
    REQUIRE(initialization.event_changes.front().transition ==
            mycore::rollback::EventTransition::AuthorityOnly);
    REQUIRE(timeline.statistics().initialization_count == 1);

    const auto advanced =
        timeline.advance(sequence(1), ToyStimulus{.delta = 2, .pulse_key = ToyEventKey{7}});
    const auto& commit = require_commit(advanced);

    REQUIRE(timeline.state()->value == 102);
    REQUIRE(timeline.state()->tick == mycore::time::Tick{11});
    REQUIRE(timeline.predicted_tick() == mycore::time::Tick{11});
    REQUIRE(timeline.history().size() == 1);
    REQUIRE(commit.kind == mycore::rollback::CommitKind::Advance);
    REQUIRE(commit.state_diff.value_delta == 2);
    REQUIRE(commit.event_changes.size() == 1);
    REQUIRE(commit.event_changes.front().transition ==
            mycore::rollback::EventTransition::FirstPredicted);

    const auto state_before_failure = timeline.state()->value;
    const auto history_before_failure = timeline.history().size();
    const auto duplicate = timeline.advance(sequence(1), ToyStimulus{.delta = 100});
    REQUIRE(require_failure(duplicate).code ==
            mycore::rollback::TimelineErrorCode::NonMonotonicCommandSequence);
    REQUIRE(timeline.state()->value == state_before_failure);
    REQUIRE(timeline.history().size() == history_before_failure);
}

TEST_CASE("Rollback rejects invalid lifecycle, bounds, authority, and scope inputs atomically",
          "[rollback][timeline]") {
    Timeline invalid_checkpoint_timeline{ToyModel{}};
    auto invalid_checkpoint = authority(0, 0);
    invalid_checkpoint.checkpoint.valid = false;
    const auto invalid_initialization =
        invalid_checkpoint_timeline.initialize(invalid_checkpoint, ToyScope{});
    REQUIRE(require_failure(invalid_initialization).code ==
            mycore::rollback::TimelineErrorCode::ModelRestoreFailed);
    REQUIRE_FALSE(invalid_checkpoint_timeline.initialized());
    REQUIRE(invalid_checkpoint_timeline.state() == nullptr);

    Timeline timeline{ToyModel{}, {.capacity = 1}};
    REQUIRE(std::holds_alternative<Commit>(timeline.initialize(authority(0, 0), ToyScope{})));

    REQUIRE(require_failure(timeline.initialize(authority(0, 0), ToyScope{})).code ==
            mycore::rollback::TimelineErrorCode::AlreadyInitialized);
    REQUIRE(require_failure(
                timeline.advance(mycore::rollback::CommandSequence::invalid(), ToyStimulus{}))
                .code == mycore::rollback::TimelineErrorCode::InvalidCommandSequence);
    REQUIRE(std::holds_alternative<Commit>(timeline.advance(sequence(1), ToyStimulus{.delta = 1})));

    const auto value_before_failures = timeline.state()->value;
    REQUIRE(require_failure(timeline.advance(sequence(2), ToyStimulus{.delta = 100})).code ==
            mycore::rollback::TimelineErrorCode::HistoryExhausted);
    REQUIRE(require_failure(timeline.reconcile(authority(0, 100))).code ==
            mycore::rollback::TimelineErrorCode::StaleAuthority);
    REQUIRE(require_failure(timeline.reconcile(authority(1, 100, sequence(2)))).code ==
            mycore::rollback::TimelineErrorCode::InvalidAcknowledgement);
    REQUIRE(require_failure(timeline.reconcile(authority(1, 100, std::nullopt, epoch(2)))).code ==
            mycore::rollback::TimelineErrorCode::IncompatibleScope);
    REQUIRE(
        require_failure(timeline.rebase_scope(authority(0, 0, std::nullopt, epoch(1)), ToyScope{}))
            .code == mycore::rollback::TimelineErrorCode::IncompatibleScope);
    REQUIRE(
        require_failure(timeline.hard_resync(authority(1, 100, std::nullopt, epoch(0)), ToyScope{}))
            .code == mycore::rollback::TimelineErrorCode::IncompatibleScope);

    REQUIRE(timeline.state()->value == value_before_failures);
    REQUIRE(timeline.authoritative_tick() == mycore::time::Tick{0});
    REQUIRE(timeline.predicted_tick() == mycore::time::Tick{1});
    REQUIRE(timeline.history().size() == 1);
    REQUIRE(mycore::rollback::timeline_error_name(
                mycore::rollback::TimelineErrorCode::HistoryExhausted) == "history_exhausted");
}

TEST_CASE("Rollback reconciliation restores authority and replays the unacknowledged suffix",
          "[rollback][timeline]") {
    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(timeline.initialize(authority(10, 0), ToyScope{})));

    REQUIRE(std::holds_alternative<Commit>(timeline.advance(sequence(1), ToyStimulus{.delta = 1})));
    REQUIRE(std::holds_alternative<Commit>(timeline.advance(sequence(2), ToyStimulus{.delta = 2})));
    REQUIRE(std::holds_alternative<Commit>(timeline.advance(sequence(3), ToyStimulus{.delta = 3})));

    const auto reconciled = timeline.reconcile(authority(11, 10, sequence(1)));
    const auto& commit = require_commit(reconciled);

    REQUIRE(commit.kind == mycore::rollback::CommitKind::Reconcile);
    REQUIRE(commit.previous_predicted_tick == mycore::time::Tick{13});
    REQUIRE(commit.authoritative_tick == mycore::time::Tick{11});
    REQUIRE(commit.predicted_tick == mycore::time::Tick{13});
    REQUIRE(commit.replayed_frame_count == 2);
    REQUIRE(commit.prior_prediction_digest_at_authority.has_value());
    REQUIRE(timeline.state()->value == 15);
    REQUIRE(timeline.history().size() == 2);
    REQUIRE(timeline.history()[0].command_sequence == sequence(2));
    REQUIRE(timeline.history()[1].command_sequence == sequence(3));
    REQUIRE(timeline.statistics().replayed_frame_count == 2);
}

TEST_CASE("Rollback reconciliation can atomically refresh derived replay stimuli",
          "[rollback][timeline][stimulus]") {
    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(timeline.initialize(authority(0, 0), ToyScope{})));
    REQUIRE(std::holds_alternative<Commit>(timeline.advance(sequence(1), ToyStimulus{.delta = 1})));
    REQUIRE(std::holds_alternative<Commit>(timeline.advance(sequence(2), ToyStimulus{.delta = 2})));

    const auto reconciled = timeline.reconcile_with_stimulus_refresh(
        authority(1, 10, sequence(1)),
        [](mycore::rollback::CommandSequence command,
           const ToyStimulus& previous,
           const ToyState& state,
           const ToyScope&) -> std::variant<ToyStimulus, ToyError> {
            REQUIRE(command == sequence(2));
            REQUIRE(previous.delta == 2);
            REQUIRE(state.value == 10);
            auto refreshed = previous;
            refreshed.derived_delta = 20;
            return refreshed;
        });
    const auto& commit = require_commit(reconciled);

    REQUIRE(commit.kind == mycore::rollback::CommitKind::Reconcile);
    REQUIRE(commit.replayed_frame_count == 1);
    REQUIRE(timeline.state()->value == 32);
    REQUIRE(timeline.history().size() == 1);
    REQUIRE(timeline.history().front().stimulus.delta == 2);
    REQUIRE(timeline.history().front().stimulus.derived_delta == 20);
}

TEST_CASE("Same-tick authority refresh publishes events without discarding future history",
          "[rollback][timeline][events]") {
    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(timeline.initialize(authority(10, 4), ToyScope{})));
    REQUIRE(std::holds_alternative<Commit>(
        timeline.advance(sequence(1), ToyStimulus{.delta = 2, .pulse_key = ToyEventKey{1}})));

    const auto refreshed = timeline.refresh_authority(
        authority(10,
                  5,
                  std::nullopt,
                  epoch(1),
                  {confirmed(ToyEvent{NoticeEvent{.key = ToyEventKey{2}, .payload = 9}})}));
    const auto& commit = require_commit(refreshed);

    REQUIRE(commit.kind == mycore::rollback::CommitKind::AuthorityRefresh);
    REQUIRE(commit.replayed_frame_count == 1);
    REQUIRE(commit.event_changes.size() == 2);
    CHECK(commit.event_changes[0].transition == mycore::rollback::EventTransition::Revised);
    CHECK(commit.event_changes[0].key == ToyEventKey{1});
    CHECK(commit.event_changes[1].transition == mycore::rollback::EventTransition::AuthorityOnly);
    CHECK(commit.event_changes[1].key == ToyEventKey{2});
    CHECK(timeline.state()->value == 7);
    REQUIRE(timeline.history().size() == 1);
    CHECK(timeline.history().front().command_sequence == sequence(1));
    CHECK(timeline.statistics().authority_refresh_count == 1);

    CHECK(require_failure(timeline.refresh_authority(authority(11, 6))).code ==
          mycore::rollback::TimelineErrorCode::IncompatibleAuthority);
    CHECK(timeline.authoritative_tick() == mycore::time::Tick{10});
    CHECK(timeline.history().size() == 1);
}

TEST_CASE("Hard resync may accept externally submitted commands beyond timeline history",
          "[rollback][timeline][recovery]") {
    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(timeline.initialize(authority(0, 0), ToyScope{})));
    REQUIRE(std::holds_alternative<Commit>(timeline.advance(sequence(1), ToyStimulus{.delta = 1})));

    const auto resynced = timeline.hard_resync(authority(2, 9, sequence(3)), ToyScope{});
    const auto& commit = require_commit(resynced);
    CHECK(commit.kind == mycore::rollback::CommitKind::HardResync);
    CHECK(commit.acknowledged_through == sequence(3));
    CHECK(timeline.last_submitted_sequence() == sequence(3));
    CHECK(timeline.history().empty());
    CHECK(timeline.state()->value == 9);
}

TEST_CASE("Failed scratch replay does not mutate committed rollback state",
          "[rollback][timeline]") {
    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(timeline.initialize(authority(0, 0), ToyScope{})));
    REQUIRE(std::holds_alternative<Commit>(
        timeline.advance(sequence(1), ToyStimulus{.delta = 1, .fail_at_or_above = 50})));

    const auto failed = timeline.reconcile(authority(1, 100));
    const auto& failure = require_failure(failed);

    REQUIRE(failure.code == mycore::rollback::TimelineErrorCode::ModelStepFailed);
    REQUIRE(failure.model_error == ToyError::StepRejected);

    auto invalid_authority = authority(1, 100);
    invalid_authority.checkpoint.valid = false;
    REQUIRE(require_failure(timeline.reconcile(invalid_authority)).code ==
            mycore::rollback::TimelineErrorCode::ModelRestoreFailed);

    REQUIRE(timeline.state()->value == 1);
    REQUIRE(timeline.authoritative_tick() == mycore::time::Tick{0});
    REQUIRE(timeline.predicted_tick() == mycore::time::Tick{1});
    REQUIRE(timeline.history().size() == 1);

    Timeline refresh_timeline{ToyModel{}};
    REQUIRE(
        std::holds_alternative<Commit>(refresh_timeline.initialize(authority(0, 0), ToyScope{})));
    REQUIRE(std::holds_alternative<Commit>(
        refresh_timeline.advance(sequence(1), ToyStimulus{.delta = 1})));
    const auto refresh_failure = refresh_timeline.reconcile_with_stimulus_refresh(
        authority(1, 10),
        [](mycore::rollback::CommandSequence, const ToyStimulus&, const ToyState&, const ToyScope&)
            -> std::variant<ToyStimulus, ToyError> {
            return ToyError::StepRejected;
        });
    REQUIRE(require_failure(refresh_failure).code ==
            mycore::rollback::TimelineErrorCode::StimulusRefreshFailed);
    REQUIRE(mycore::rollback::timeline_error_name(
                mycore::rollback::TimelineErrorCode::StimulusRefreshFailed) ==
            "stimulus_refresh_failed");
    REQUIRE(refresh_timeline.state()->value == 1);
    REQUIRE(refresh_timeline.authoritative_tick() == mycore::time::Tick{0});
    REQUIRE(refresh_timeline.history().front().stimulus.delta == 1);
}

TEST_CASE("Rollback scope rebasing replays retained stimuli with the new scope",
          "[rollback][scope]") {
    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(
        timeline.initialize(authority(0, 0), ToyScope{.multiplier = 1})));
    REQUIRE(std::holds_alternative<Commit>(timeline.advance(sequence(1), ToyStimulus{.delta = 2})));

    const auto rebased =
        timeline.rebase_scope(authority(0, 0, std::nullopt, epoch(2)), ToyScope{.multiplier = 10});
    const auto& commit = require_commit(rebased);

    REQUIRE(commit.kind == mycore::rollback::CommitKind::ScopeRebase);
    REQUIRE(commit.replayed_frame_count == 1);
    REQUIRE(timeline.state()->value == 20);
    REQUIRE(timeline.scope_epoch() == epoch(2));
    REQUIRE(timeline.history().front().scope_epoch == epoch(2));
}

TEST_CASE("Rollback scope rebasing can refresh stimuli for the new scope",
          "[rollback][scope][stimulus]") {
    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(
        timeline.initialize(authority(0, 0), ToyScope{.multiplier = 1})));
    REQUIRE(std::holds_alternative<Commit>(timeline.advance(sequence(1), ToyStimulus{.delta = 2})));

    const auto rebased = timeline.rebase_scope_with_stimulus_refresh(
        authority(0, 0, std::nullopt, epoch(2)),
        ToyScope{.multiplier = 10},
        [](mycore::rollback::CommandSequence,
           const ToyStimulus& previous,
           const ToyState& state,
           const ToyScope& scope) -> std::variant<ToyStimulus, ToyError> {
            REQUIRE(state.value == 0);
            REQUIRE(scope.multiplier == 10);
            auto refreshed = previous;
            refreshed.derived_delta = 1;
            return refreshed;
        });
    const auto& commit = require_commit(rebased);

    REQUIRE(commit.kind == mycore::rollback::CommitKind::ScopeRebase);
    REQUIRE(commit.replayed_frame_count == 1);
    REQUIRE(timeline.state()->value == 30);
    REQUIRE(timeline.history().front().stimulus.delta == 2);
    REQUIRE(timeline.history().front().stimulus.derived_delta == 1);
}

TEST_CASE("Rollback reports revised, confirmed, rejected, and authority-only events",
          "[rollback][events]") {
    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(timeline.initialize(authority(0, 0), ToyScope{})));
    REQUIRE(std::holds_alternative<Commit>(
        timeline.advance(sequence(1), ToyStimulus{.delta = 1, .pulse_key = ToyEventKey{1}})));

    const auto revised = timeline.reconcile(authority(1, 5));
    const auto& revised_commit = require_commit(revised);
    REQUIRE(revised_commit.event_changes.size() == 1);
    REQUIRE(revised_commit.event_changes.front().transition ==
            mycore::rollback::EventTransition::Revised);
    REQUIRE(std::get<PulseEvent>(*revised_commit.event_changes.front().current).payload == 6);

    const auto confirmed_event = ToyEvent{PulseEvent{.key = ToyEventKey{1}, .payload = 6}};
    const auto confirmation =
        timeline.reconcile(authority(2, 6, sequence(1), epoch(1), {confirmed(confirmed_event)}));
    const auto& confirmation_commit = require_commit(confirmation);
    REQUIRE(confirmation_commit.event_changes.size() == 1);
    REQUIRE(confirmation_commit.event_changes.front().transition ==
            mycore::rollback::EventTransition::Confirmed);

    const auto authority_only = timeline.reconcile(
        authority(3,
                  6,
                  sequence(1),
                  epoch(1),
                  {confirmed(ToyEvent{PulseEvent{.key = ToyEventKey{2}, .payload = 9}})}));
    const auto& authority_only_commit = require_commit(authority_only);
    REQUIRE(authority_only_commit.event_changes.size() == 1);
    REQUIRE(authority_only_commit.event_changes.front().transition ==
            mycore::rollback::EventTransition::AuthorityOnly);

    REQUIRE(std::holds_alternative<Commit>(
        timeline.advance(sequence(2), ToyStimulus{.delta = 0, .pulse_key = ToyEventKey{3}})));
    const auto rejection =
        timeline.reconcile(authority(4, 6, sequence(2), epoch(1), {rejected(ToyEventKey{3})}));
    const auto& rejection_commit = require_commit(rejection);
    REQUIRE(rejection_commit.event_changes.size() == 1);
    REQUIRE(rejection_commit.event_changes.front().transition ==
            mycore::rollback::EventTransition::Retracted);
}

TEST_CASE("Duplicate event identities fail without mutating the timeline", "[rollback][events]") {
    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(timeline.initialize(authority(0, 0), ToyScope{})));
    REQUIRE(std::holds_alternative<Commit>(
        timeline.advance(sequence(1), ToyStimulus{.delta = 1, .pulse_key = ToyEventKey{1}})));

    const auto duplicate =
        timeline.advance(sequence(2), ToyStimulus{.delta = 1, .pulse_key = ToyEventKey{1}});
    REQUIRE(require_failure(duplicate).code ==
            mycore::rollback::TimelineErrorCode::DuplicateEventKey);
    REQUIRE(timeline.state()->value == 1);
    REQUIRE(timeline.history().size() == 1);
}

TEST_CASE("Authoritative event conflicts fail and identical duplicates coalesce",
          "[rollback][events]") {
    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(timeline.initialize(authority(0, 0), ToyScope{})));
    REQUIRE(std::holds_alternative<Commit>(
        timeline.advance(sequence(1), ToyStimulus{.delta = 1, .pulse_key = ToyEventKey{1}})));

    auto conflicting = confirmed(ToyEvent{PulseEvent{.key = ToyEventKey{1}, .payload = 1}});
    conflicting.key = ToyEventKey{2};
    REQUIRE(
        require_failure(timeline.reconcile(authority(1, 1, sequence(1), epoch(1), {conflicting})))
            .code == mycore::rollback::TimelineErrorCode::ConflictingAuthorityEvent);
    REQUIRE(timeline.state()->value == 1);
    REQUIRE(timeline.authoritative_tick() == mycore::time::Tick{0});
    REQUIRE(timeline.predicted_tick() == mycore::time::Tick{1});
    REQUIRE(timeline.history().size() == 1);

    const auto duplicate_event =
        confirmed(ToyEvent{PulseEvent{.key = ToyEventKey{1}, .payload = 1}});
    const auto conflicting_duplicate =
        confirmed(ToyEvent{PulseEvent{.key = ToyEventKey{1}, .payload = 2}});
    REQUIRE(
        require_failure(timeline.reconcile(authority(
                            1, 1, sequence(1), epoch(1), {duplicate_event, conflicting_duplicate})))
            .code == mycore::rollback::TimelineErrorCode::ConflictingAuthorityEvent);

    const auto& coalesced = require_commit(timeline.reconcile(
        authority(1, 1, sequence(1), epoch(1), {duplicate_event, duplicate_event})));
    REQUIRE(coalesced.event_changes.size() == 1);
    REQUIRE(coalesced.event_changes.front().transition ==
            mycore::rollback::EventTransition::Confirmed);
    REQUIRE(timeline.state()->value == 1);
    REQUIRE(timeline.authoritative_tick() == mycore::time::Tick{1});
    REQUIRE(timeline.predicted_tick() == mycore::time::Tick{1});
    REQUIRE(timeline.history().empty());
}

TEST_CASE("Hard resync clears history and retracts speculative events", "[rollback][recovery]") {
    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(timeline.initialize(authority(0, 0), ToyScope{})));
    REQUIRE(std::holds_alternative<Commit>(
        timeline.advance(sequence(1), ToyStimulus{.delta = 2, .pulse_key = ToyEventKey{4}})));

    const auto resynced =
        timeline.hard_resync(authority(0, 20, std::nullopt, epoch(2)), ToyScope{});
    const auto& commit = require_commit(resynced);

    REQUIRE(commit.kind == mycore::rollback::CommitKind::HardResync);
    REQUIRE(commit.event_changes.size() == 1);
    REQUIRE(commit.event_changes.front().transition ==
            mycore::rollback::EventTransition::Retracted);
    REQUIRE(timeline.state()->value == 20);
    REQUIRE(timeline.history().empty());
    REQUIRE(timeline.statistics().hard_resync_count == 1);
}

TEST_CASE("Rollback event transitions preserve deterministic journal order", "[rollback][events]") {
    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(timeline.initialize(authority(0, 0), ToyScope{})));
    REQUIRE(std::holds_alternative<Commit>(timeline.advance(
        sequence(1), ToyStimulus{.pulse_key = ToyEventKey{1}, .notice_key = ToyEventKey{2}})));
    REQUIRE(std::holds_alternative<Commit>(
        timeline.advance(sequence(2), ToyStimulus{.pulse_key = ToyEventKey{3}})));

    const auto& resynced =
        require_commit(timeline.hard_resync(authority(0, 0, std::nullopt, epoch(2)), ToyScope{}));

    REQUIRE(resynced.event_changes.size() == 3);
    REQUIRE(resynced.event_changes[0].key == ToyEventKey{1});
    REQUIRE(resynced.event_changes[1].key == ToyEventKey{2});
    REQUIRE(resynced.event_changes[2].key == ToyEventKey{3});
    for (const auto& change : resynced.event_changes) {
        REQUIRE(change.transition == mycore::rollback::EventTransition::Retracted);
    }
    REQUIRE(resynced.retired_event_keys ==
            std::vector<ToyEventKey>{ToyEventKey{1}, ToyEventKey{2}, ToyEventKey{3}});
}

TEST_CASE("Consequence policies deliver, revise, cancel, confirm, and suppress by key",
          "[rollback][consequences]") {
    HandlerLog once_log;
    HandlerLog cancelable_log;
    HandlerLog confirmed_log;
    mycore::rollback::StaticConsequenceRouter<ToyModel,
                                              PredictOncePulseHandler,
                                              CancelablePulseHandler,
                                              ConfirmOncePulseHandler>
        router{PredictOncePulseHandler{.log = &once_log},
               CancelablePulseHandler{.log = &cancelable_log},
               ConfirmOncePulseHandler{.log = &confirmed_log}};

    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(timeline.initialize(authority(0, 0), ToyScope{})));

    const auto& predicted = require_commit(
        timeline.advance(sequence(1), ToyStimulus{.delta = 1, .pulse_key = ToyEventKey{1}}));
    const auto predicted_report = router.consume(predicted);
    REQUIRE(predicted_report.failures.empty());
    REQUIRE(predicted_report.handlers.size() == 3);
    CHECK(predicted_report.handlers[0].handler_index == 0);
    CHECK(predicted_report.handlers[0].policy == mycore::rollback::ConsequencePolicy::PredictOnce);
    CHECK(predicted_report.handlers[0].statistics.delivered_count == 1);
    CHECK(predicted_report.handlers[1].policy ==
          mycore::rollback::ConsequencePolicy::PredictCancelable);
    CHECK(predicted_report.handlers[1].statistics.delivered_count == 1);
    CHECK(predicted_report.handlers[2].policy == mycore::rollback::ConsequencePolicy::ConfirmOnce);
    CHECK(predicted_report.handlers[2].statistics.suppressed_count == 1);
    REQUIRE(once_log.first.size() == 1);
    REQUIRE(cancelable_log.predicted.size() == 1);
    REQUIRE(confirmed_log.confirmed.empty());

    const auto& revised = require_commit(timeline.reconcile(authority(1, 5)));
    REQUIRE(router.consume(revised).failures.empty());
    REQUIRE(once_log.first.size() == 1);
    REQUIRE(cancelable_log.revised.size() == 1);

    const auto event = ToyEvent{PulseEvent{.key = ToyEventKey{1}, .payload = 6}};
    const auto& confirmed_commit = require_commit(
        timeline.reconcile(authority(2, 6, sequence(1), epoch(1), {confirmed(event)})));
    REQUIRE(router.consume(confirmed_commit).failures.empty());
    REQUIRE(once_log.first.size() == 1);
    REQUIRE(cancelable_log.confirmed.size() == 1);
    REQUIRE(confirmed_log.confirmed.size() == 1);

    const auto& repeated = require_commit(
        timeline.reconcile(authority(3, 6, sequence(1), epoch(1), {confirmed(event)})));
    const auto repeated_report = router.consume(repeated);
    REQUIRE(once_log.first.size() == 1);
    REQUIRE(cancelable_log.predicted.size() == 1);
    REQUIRE(cancelable_log.confirmed.size() == 1);
    REQUIRE(confirmed_log.confirmed.size() == 1);
    REQUIRE(repeated_report.statistics.suppressed_count == 3);
    REQUIRE(repeated_report.handlers.size() == 3);
    CHECK(repeated_report.handlers[0].statistics.suppressed_count == 1);
    CHECK(repeated_report.handlers[1].statistics.suppressed_count == 1);
    CHECK(repeated_report.handlers[2].statistics.suppressed_count == 1);
    const auto cumulative_handlers = router.handler_statistics();
    REQUIRE(cumulative_handlers.size() == 3);
    CHECK(cumulative_handlers[0].statistics.delivered_count == 1);
    CHECK(cumulative_handlers[1].statistics.revised_count == 1);
    CHECK(cumulative_handlers[1].statistics.confirmed_count == 1);
    CHECK(cumulative_handlers[2].statistics.confirmed_count == 1);

    const auto authority_only_event = ToyEvent{PulseEvent{.key = ToyEventKey{4}, .payload = 12}};
    const auto& authority_only = require_commit(timeline.reconcile(
        authority(4, 6, sequence(1), epoch(1), {confirmed(authority_only_event)})));
    REQUIRE(router.consume(authority_only).failures.empty());
    REQUIRE(once_log.first.size() == 2);
    REQUIRE(cancelable_log.predicted.size() == 2);
    REQUIRE(cancelable_log.confirmed.size() == 2);
    REQUIRE(confirmed_log.confirmed.size() == 2);

    const auto& second_predicted =
        require_commit(timeline.advance(sequence(2), ToyStimulus{.pulse_key = ToyEventKey{2}}));
    REQUIRE(router.consume(second_predicted).failures.empty());
    const auto& retracted = require_commit(
        timeline.reconcile(authority(5, 6, sequence(2), epoch(1), {rejected(ToyEventKey{2})})));
    REQUIRE(router.consume(retracted).failures.empty());
    REQUIRE(cancelable_log.canceled.size() == 1);

    const auto once_count = once_log.first.size();
    const auto cancelable_count = cancelable_log.predicted.size();
    const auto confirmed_count = confirmed_log.confirmed.size();
    const auto& unrelated =
        require_commit(timeline.advance(sequence(3), ToyStimulus{.notice_key = ToyEventKey{8}}));
    const auto unrelated_report = router.consume(unrelated);
    REQUIRE(unrelated_report.statistics.delivered_count == 0);
    REQUIRE(once_log.first.size() == once_count);
    REQUIRE(cancelable_log.predicted.size() == cancelable_count);
    REQUIRE(confirmed_log.confirmed.size() == confirmed_count);
}

TEST_CASE("Failed consequence delivery is not retried for the same occurrence",
          "[rollback][consequences]") {
    HandlerLog log;
    mycore::rollback::StaticConsequenceRouter<ToyModel, PredictOncePulseHandler> router{
        PredictOncePulseHandler{.log = &log, .fail = true}};

    Timeline timeline{ToyModel{}};
    REQUIRE(std::holds_alternative<Commit>(timeline.initialize(authority(0, 0), ToyScope{})));
    const auto event = ToyEvent{PulseEvent{.key = ToyEventKey{9}, .payload = 1}};

    const auto& predicted = require_commit(
        timeline.advance(sequence(1), ToyStimulus{.delta = 1, .pulse_key = ToyEventKey{9}}));
    const auto first_report = router.consume(predicted);
    REQUIRE(first_report.failures.size() == 1);
    REQUIRE(log.first.size() == 1);

    const auto& confirmed_commit = require_commit(
        timeline.reconcile(authority(1, 1, sequence(1), epoch(1), {confirmed(event)})));
    const auto second_report = router.consume(confirmed_commit);
    REQUIRE(log.first.size() == 1);
    REQUIRE(second_report.statistics.suppressed_count == 1);
}

TEST_CASE("Authority-only events use the same consequence batch contract",
          "[rollback][events][consequences]") {
    const auto event = ToyEvent{PulseEvent{.key = ToyEventKey{12}, .payload = 8}};
    const std::vector events{confirmed(event)};
    auto resolved = mycore::rollback::resolve_authority_only_events<ToyModel>(ToyModel{}, events);
    auto* batch = std::get_if<mycore::rollback::EventBatch<ToyModel>>(&resolved);
    REQUIRE(batch != nullptr);
    REQUIRE(batch->kind == mycore::rollback::CommitKind::AuthorityOnly);
    REQUIRE(batch->changes.size() == 1);
    CHECK(batch->changes.front().transition == mycore::rollback::EventTransition::AuthorityOnly);
    CHECK(batch->retired_keys == std::vector{ToyEventKey{12}});

    HandlerLog log;
    mycore::rollback::StaticConsequenceRouter<ToyModel, ConfirmOncePulseHandler> router{
        ConfirmOncePulseHandler{.log = &log}};
    REQUIRE(router.consume(*batch).failures.empty());
    REQUIRE(log.confirmed.size() == 1);
    const auto repeated = router.consume(*batch);
    REQUIRE(repeated.statistics.suppressed_count == 1);
    REQUIRE(log.confirmed.size() == 1);

    auto conflicting = events;
    conflicting.front().key = ToyEventKey{99};
    CHECK(std::get<mycore::rollback::TimelineErrorCode>(
              mycore::rollback::resolve_authority_only_events<ToyModel>(ToyModel{}, conflicting)) ==
          mycore::rollback::TimelineErrorCode::ConflictingAuthorityEvent);

    const std::vector rejected_events{rejected(ToyEventKey{13})};
    auto rejected_result =
        mycore::rollback::resolve_authority_only_events<ToyModel>(ToyModel{}, rejected_events);
    const auto* rejected_batch =
        std::get_if<mycore::rollback::EventBatch<ToyModel>>(&rejected_result);
    REQUIRE(rejected_batch != nullptr);
    CHECK(rejected_batch->changes.empty());
    CHECK(rejected_batch->retired_keys == std::vector{ToyEventKey{13}});
}

TEST_CASE("Consequence ledgers require replay and external retirement evidence",
          "[rollback][consequences][retirement]") {
    HandlerLog once_log;
    HandlerLog cancelable_log;
    HandlerLog confirmed_log;
    mycore::rollback::StaticConsequenceRouter<ToyModel,
                                              PredictOncePulseHandler,
                                              CancelablePulseHandler,
                                              ConfirmOncePulseHandler>
        router{PredictOncePulseHandler{.log = &once_log},
               CancelablePulseHandler{.log = &cancelable_log},
               ConfirmOncePulseHandler{.log = &confirmed_log}};
    const auto key = ToyEventKey{21};
    const auto event = ToyEvent{PulseEvent{.key = key, .payload = 7}};

    const mycore::rollback::EventBatch<ToyModel> predicted{
        .kind = mycore::rollback::CommitKind::Advance,
        .changes = {{
            .transition = mycore::rollback::EventTransition::FirstPredicted,
            .key = key,
            .previous = std::nullopt,
            .current = event,
        }},
        .retired_keys = {},
        .externally_retired_keys = {},
    };
    REQUIRE(router.consume(predicted).failures.empty());
    CHECK(router.ledger_statistics().predict_once_key_count == 1);
    CHECK(router.ledger_statistics().cancelable_active_key_count == 1);

    const mycore::rollback::EventBatch<ToyModel> replay_retired{
        .kind = mycore::rollback::CommitKind::Reconcile,
        .changes = {},
        .retired_keys = {key},
        .externally_retired_keys = {},
    };
    REQUIRE(router.consume(replay_retired).failures.empty());
    CHECK(router.ledger_statistics().replay_retirement_evidence_count == 1);
    CHECK(router.ledger_statistics().predict_once_key_count == 1);

    const mycore::rollback::EventBatch<ToyModel> externally_retired{
        .kind = mycore::rollback::CommitKind::AuthorityOnly,
        .changes = {},
        .retired_keys = {},
        .externally_retired_keys = {key},
    };
    REQUIRE(router.consume(externally_retired).failures.empty());
    CHECK(router.ledger_statistics().external_retirement_evidence_count == 1);
    CHECK(router.ledger_statistics().pruned_occurrence_count == 0);

    const mycore::rollback::EventBatch<ToyModel> confirmed_batch{
        .kind = mycore::rollback::CommitKind::Reconcile,
        .changes = {{
            .transition = mycore::rollback::EventTransition::Confirmed,
            .key = key,
            .previous = event,
            .current = event,
        }},
        .retired_keys = {},
        .externally_retired_keys = {},
    };
    REQUIRE(router.consume(confirmed_batch).failures.empty());
    const auto ledger = router.ledger_statistics();
    CHECK(ledger.replay_retirement_evidence_count == 0);
    CHECK(ledger.external_retirement_evidence_count == 0);
    CHECK(ledger.predict_once_key_count == 0);
    CHECK(ledger.confirm_once_key_count == 0);
    CHECK(ledger.cancelable_active_key_count == 0);
    CHECK(ledger.cancelable_inactive_key_count == 0);
    CHECK(ledger.pruned_occurrence_count == 1);
    CHECK(once_log.first.size() == 1);
    CHECK(cancelable_log.predicted.size() == 1);
    CHECK(cancelable_log.confirmed.size() == 1);
    CHECK(confirmed_log.confirmed.size() == 1);
}

TEST_CASE("Consequence retirement remains bounded across many confirmed occurrences",
          "[rollback][consequences][retirement]") {
    HandlerLog once_log;
    HandlerLog cancelable_log;
    HandlerLog confirmed_log;
    mycore::rollback::StaticConsequenceRouter<ToyModel,
                                              PredictOncePulseHandler,
                                              CancelablePulseHandler,
                                              ConfirmOncePulseHandler>
        router{PredictOncePulseHandler{.log = &once_log},
               CancelablePulseHandler{.log = &cancelable_log},
               ConfirmOncePulseHandler{.log = &confirmed_log}};

    constexpr auto kOccurrenceCount = std::uint32_t{4096};
    for (auto value = std::uint32_t{1}; value <= kOccurrenceCount; ++value) {
        const auto key = ToyEventKey{value};
        const auto event =
            ToyEvent{PulseEvent{.key = key, .payload = static_cast<std::int32_t>(value)}};
        const mycore::rollback::EventBatch<ToyModel> batch{
            .kind = mycore::rollback::CommitKind::Reconcile,
            .changes = {{
                .transition = mycore::rollback::EventTransition::Confirmed,
                .key = key,
                .previous = std::nullopt,
                .current = event,
            }},
            .retired_keys = {key},
            .externally_retired_keys = {key},
        };
        REQUIRE(router.consume(batch).failures.empty());
    }

    const auto ledger = router.ledger_statistics();
    CHECK(ledger.replay_retirement_evidence_count == 0);
    CHECK(ledger.external_retirement_evidence_count == 0);
    CHECK(ledger.predict_once_key_count == 0);
    CHECK(ledger.confirm_once_key_count == 0);
    CHECK(ledger.cancelable_active_key_count == 0);
    CHECK(ledger.cancelable_inactive_key_count == 0);
    CHECK(ledger.pruned_occurrence_count == static_cast<std::uint64_t>(kOccurrenceCount));
    CHECK(once_log.first.size() == static_cast<std::size_t>(kOccurrenceCount));
    CHECK(cancelable_log.predicted.size() == static_cast<std::size_t>(kOccurrenceCount));
    CHECK(cancelable_log.confirmed.size() == static_cast<std::size_t>(kOccurrenceCount));
    CHECK(confirmed_log.confirmed.size() == static_cast<std::size_t>(kOccurrenceCount));
}

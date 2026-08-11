#include "mycore/rollback/rollback.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

struct FuzzEventKey {
    std::uint8_t type{};
    std::uint8_t occurrence{};

    auto operator<=>(const FuzzEventKey&) const = default;
};

struct PulseEvent {
    std::uint8_t occurrence{};
    std::int32_t payload{};

    auto operator<=>(const PulseEvent&) const = default;
};

struct NoticeEvent {
    std::uint8_t occurrence{};
    std::int32_t payload{};

    auto operator<=>(const NoticeEvent&) const = default;
};

using FuzzEvent = std::variant<PulseEvent, NoticeEvent>;

struct FuzzEventKeyHash {
    [[nodiscard]] std::size_t operator()(FuzzEventKey key) const noexcept {
        return (static_cast<std::size_t>(key.type) << 8U) | key.occurrence;
    }
};

struct FuzzScope {
    std::uint8_t multiplier{1};

    auto operator<=>(const FuzzScope&) const = default;
};

struct FuzzState {
    mycore::time::Tick tick;
    std::int64_t value{};

    auto operator<=>(const FuzzState&) const = default;
};

struct FuzzCheckpoint {
    mycore::time::Tick tick;
    std::int64_t value{};
    bool valid{true};

    auto operator<=>(const FuzzCheckpoint&) const = default;
};

struct FuzzStimulus {
    std::int8_t delta{};
    std::uint8_t event_type{};
    std::uint8_t occurrence{};
    bool fail{};

    auto operator<=>(const FuzzStimulus&) const = default;
};

struct FuzzStateDiff {
    std::int64_t delta{};

    auto operator<=>(const FuzzStateDiff&) const = default;
};

enum class FuzzError : std::uint8_t {
    InvalidCheckpoint,
    InvalidScope,
    StepRejected,
};

struct FuzzModel {
    using State = FuzzState;
    using Checkpoint = FuzzCheckpoint;
    using Stimulus = FuzzStimulus;
    using Scope = FuzzScope;
    using Event = FuzzEvent;
    using EventKey = FuzzEventKey;
    using EventKeyHash = FuzzEventKeyHash;
    using StateDiff = FuzzStateDiff;
    using StateDigest = std::uint64_t;
    using Error = FuzzError;

    [[nodiscard]] std::variant<State, Error> restore(const Checkpoint& checkpoint,
                                                     const Scope& scope) const {
        if (!checkpoint.valid) {
            return Error::InvalidCheckpoint;
        }
        if (scope.multiplier == 0 || scope.multiplier > 3) {
            return Error::InvalidScope;
        }
        return State{.tick = checkpoint.tick, .value = checkpoint.value};
    }

    [[nodiscard]] Checkpoint capture(const State& state, const Scope&) const {
        return {.tick = state.tick, .value = state.value, .valid = true};
    }

    [[nodiscard]] std::variant<std::vector<Event>, Error>
    step(State& state, const Stimulus& stimulus, const Scope& scope) const {
        if (stimulus.fail) {
            return Error::StepRejected;
        }
        state.value += static_cast<std::int64_t>(stimulus.delta) * scope.multiplier;
        state.tick += mycore::time::TickDelta{1};
        std::vector<Event> events;
        if (stimulus.event_type == 1U) {
            events.emplace_back(PulseEvent{.occurrence = stimulus.occurrence,
                                           .payload = static_cast<std::int32_t>(state.value)});
        } else if (stimulus.event_type == 2U) {
            events.emplace_back(NoticeEvent{.occurrence = stimulus.occurrence,
                                            .payload = static_cast<std::int32_t>(state.value)});
        }
        return events;
    }

    [[nodiscard]] StateDigest digest(const Checkpoint& checkpoint, const Scope& scope) const {
        auto result = checkpoint.tick.value() * 0x9E37'79B9U;
        result ^= static_cast<std::uint64_t>(checkpoint.value);
        result ^= static_cast<std::uint64_t>(scope.multiplier) << 56U;
        return result;
    }

    [[nodiscard]] StateDiff diff(const State& previous, const State& current, const Scope&) const {
        return {.delta = current.value - previous.value};
    }

    [[nodiscard]] EventKey event_key(const Event& event) const {
        return std::visit(
            [](const auto& value) {
                using Type = std::decay_t<decltype(value)>;
                return EventKey{.type = std::same_as<Type, PulseEvent> ? std::uint8_t{1}
                                                                       : std::uint8_t{2},
                                .occurrence = value.occurrence};
            },
            event);
    }
};

static_assert(mycore::rollback::RollbackModel<FuzzModel>);

using Timeline = mycore::rollback::Timeline<FuzzModel>;
using AuthorityFrame = mycore::rollback::AuthorityFrame<FuzzModel>;
using AuthorityEvent = mycore::rollback::AuthorityEvent<FuzzModel>;
using Commit = mycore::rollback::Commit<FuzzModel>;
using CommitResult = mycore::rollback::CommitResult<FuzzModel>;

struct PulseHandler {
    using Event = PulseEvent;
    using Token = std::uint32_t;
    static constexpr auto policy = mycore::rollback::ConsequencePolicy::PredictCancelable;

    [[nodiscard]] std::optional<Token> on_predict(const FuzzEventKey& key, const Event&) {
        ++predicted;
        return key.occurrence;
    }
    [[nodiscard]] bool on_revise(Token&, const FuzzEventKey&, const Event&) {
        ++revised;
        return true;
    }
    [[nodiscard]] bool on_cancel(Token&, const FuzzEventKey&, const Event&) {
        ++canceled;
        return true;
    }
    [[nodiscard]] bool on_confirm(Token&, const FuzzEventKey&, const Event&) {
        ++confirmed;
        return true;
    }

    std::uint64_t predicted{};
    std::uint64_t revised{};
    std::uint64_t canceled{};
    std::uint64_t confirmed{};
};

struct NoticeHandler {
    using Event = NoticeEvent;
    static constexpr auto policy = mycore::rollback::ConsequencePolicy::ConfirmOnce;

    [[nodiscard]] bool on_confirmed(const FuzzEventKey&, const Event&) {
        ++confirmed;
        return true;
    }

    std::uint64_t confirmed{};
};

using Router = mycore::rollback::StaticConsequenceRouter<FuzzModel, PulseHandler, NoticeHandler>;

[[noreturn]] void invariant_failed() noexcept {
    std::abort();
}

void require(bool condition) noexcept {
    if (!condition) {
        invariant_failed();
    }
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> input)
        : input_(input) {}

    [[nodiscard]] std::uint8_t byte() noexcept {
        if (offset_ == input_.size()) {
            return 0;
        }
        return input_[offset_++];
    }

    [[nodiscard]] bool flag() noexcept {
        return (byte() & 1U) != 0U;
    }

private:
    std::span<const std::uint8_t> input_;
    std::size_t offset_{};
};

struct FrameSummary {
    mycore::time::Tick tick;
    mycore::rollback::CommandSequence sequence;
    FuzzStimulus stimulus;
    FuzzCheckpoint checkpoint;
    FuzzModel::StateDigest digest{};
    std::vector<FuzzEvent> events;
    mycore::rollback::ScopeEpoch epoch;

    bool operator==(const FrameSummary&) const = default;
};

struct TimelineSummary {
    bool initialized{};
    std::optional<FuzzState> state;
    std::optional<FuzzScope> scope;
    std::vector<FrameSummary> history;
    mycore::time::Tick authority_tick;
    mycore::time::Tick predicted_tick;
    mycore::rollback::ScopeEpoch scope_epoch;
    std::optional<mycore::rollback::CommandSequence> acknowledged;
    std::optional<mycore::rollback::CommandSequence> last_submitted;
    mycore::rollback::TimelineStatistics statistics;
};

[[nodiscard]] TimelineSummary summarize(const Timeline& timeline) {
    TimelineSummary result{
        .initialized = timeline.initialized(),
        .state = timeline.state() != nullptr ? std::optional{*timeline.state()} : std::nullopt,
        .scope = timeline.scope() != nullptr ? std::optional{*timeline.scope()} : std::nullopt,
        .history = {},
        .authority_tick = timeline.authoritative_tick(),
        .predicted_tick = timeline.predicted_tick(),
        .scope_epoch = timeline.scope_epoch(),
        .acknowledged = timeline.acknowledged_through(),
        .last_submitted = timeline.last_submitted_sequence(),
        .statistics = timeline.statistics(),
    };
    result.history.reserve(timeline.history().size());
    for (const auto& frame : timeline.history()) {
        result.history.push_back({
            .tick = frame.tick,
            .sequence = frame.command_sequence,
            .stimulus = frame.stimulus,
            .checkpoint = frame.checkpoint,
            .digest = frame.digest,
            .events = frame.events,
            .epoch = frame.scope_epoch,
        });
    }
    return result;
}

[[nodiscard]] bool same_statistics(const mycore::rollback::TimelineStatistics& lhs,
                                   const mycore::rollback::TimelineStatistics& rhs) noexcept {
    return std::tie(lhs.initialization_count,
                    lhs.advance_count,
                    lhs.reconciliation_count,
                    lhs.authority_refresh_count,
                    lhs.scope_rebase_count,
                    lhs.hard_resync_count,
                    lhs.replayed_frame_count,
                    lhs.failure_count,
                    lhs.history_high_water) == std::tie(rhs.initialization_count,
                                                        rhs.advance_count,
                                                        rhs.reconciliation_count,
                                                        rhs.authority_refresh_count,
                                                        rhs.scope_rebase_count,
                                                        rhs.hard_resync_count,
                                                        rhs.replayed_frame_count,
                                                        rhs.failure_count,
                                                        rhs.history_high_water);
}

[[nodiscard]] bool same_summary(const TimelineSummary& lhs, const TimelineSummary& rhs) noexcept {
    return lhs.initialized == rhs.initialized && lhs.state == rhs.state && lhs.scope == rhs.scope &&
           lhs.history == rhs.history && lhs.authority_tick == rhs.authority_tick &&
           lhs.predicted_tick == rhs.predicted_tick && lhs.scope_epoch == rhs.scope_epoch &&
           lhs.acknowledged == rhs.acknowledged && lhs.last_submitted == rhs.last_submitted &&
           same_statistics(lhs.statistics, rhs.statistics);
}

void check_timeline_invariants(const Timeline& timeline) {
    if (!timeline.initialized()) {
        require(timeline.state() == nullptr);
        require(timeline.scope() == nullptr);
        require(timeline.history().empty());
        return;
    }

    require(timeline.state() != nullptr);
    require(timeline.scope() != nullptr);
    require(timeline.scope_epoch().is_valid());
    require(timeline.state()->tick == timeline.predicted_tick());
    require(timeline.predicted_tick() >= timeline.authoritative_tick());
    require(timeline.predicted_tick().value() - timeline.authoritative_tick().value() ==
            timeline.history().size());
    require(timeline.statistics().history_high_water >= timeline.history().size());
    auto expected_tick = timeline.authoritative_tick().value() + 1U;
    auto previous_sequence = timeline.acknowledged_through();
    for (const auto& frame : timeline.history()) {
        require(frame.tick == mycore::time::Tick{expected_tick++});
        require(frame.scope_epoch == timeline.scope_epoch());
        require(frame.command_sequence.is_valid());
        if (previous_sequence) {
            require(frame.command_sequence > *previous_sequence);
        }
        previous_sequence = frame.command_sequence;
    }
    if (!timeline.history().empty()) {
        require(timeline.last_submitted_sequence() == timeline.history().back().command_sequence);
        require(timeline.history().back().checkpoint ==
                FuzzModel{}.capture(*timeline.state(), *timeline.scope()));
    }
}

void check_commit(const Timeline& timeline, const Commit& commit) {
    require(timeline.initialized());
    require(commit.authoritative_tick == timeline.authoritative_tick());
    require(commit.predicted_tick == timeline.predicted_tick());
    require(commit.acknowledged_through == timeline.acknowledged_through());
    require(commit.predicted_digest ==
            FuzzModel{}.digest(FuzzModel{}.capture(*timeline.state(), *timeline.scope()),
                               *timeline.scope()));
}

[[nodiscard]] std::optional<mycore::rollback::CommandSequence>
acknowledgement(Reader& reader, const Timeline& timeline, bool allow_beyond) {
    switch (reader.byte() % 4U) {
    case 0:
        return std::nullopt;
    case 1:
        return timeline.acknowledged_through();
    case 2:
        return timeline.last_submitted_sequence();
    default: {
        const auto base = timeline.last_submitted_sequence()
                              ? timeline.last_submitted_sequence()->value()
                              : std::uint64_t{};
        return allow_beyond ? mycore::rollback::CommandSequence{base + 1U}
                            : mycore::rollback::CommandSequence{base + 17U};
    }
    }
}

[[nodiscard]] FuzzEvent event(Reader& reader) {
    const auto occurrence = static_cast<std::uint8_t>(reader.byte() % 16U);
    if ((reader.byte() & 1U) == 0U) {
        return PulseEvent{.occurrence = occurrence, .payload = reader.byte()};
    }
    return NoticeEvent{.occurrence = occurrence, .payload = reader.byte()};
}

[[nodiscard]] std::vector<AuthorityEvent> authority_events(Reader& reader) {
    if (!reader.flag()) {
        return {};
    }
    auto value = event(reader);
    auto key = FuzzModel{}.event_key(value);
    if (reader.flag()) {
        key.occurrence ^= 1U;
    }
    if (reader.flag()) {
        return {{
            .disposition = mycore::rollback::AuthorityEventDisposition::Confirmed,
            .key = key,
            .event = value,
        }};
    }
    return {{
        .disposition = mycore::rollback::AuthorityEventDisposition::Rejected,
        .key = key,
        .event = reader.flag() ? std::optional<FuzzEvent>{value} : std::nullopt,
    }};
}

[[nodiscard]] AuthorityFrame
frame(Reader& reader, const Timeline& timeline, bool same_tick, bool allow_ack_beyond) {
    const auto base_tick = timeline.authoritative_tick().value();
    const auto tick = same_tick ? base_tick : base_tick + (reader.byte() % 4U);
    const auto current_epoch = timeline.scope_epoch().value();
    const auto epoch_delta = reader.byte() % 3U;
    return {
        .tick = mycore::time::Tick{tick},
        .acknowledged_through = acknowledgement(reader, timeline, allow_ack_beyond),
        .scope_epoch = mycore::rollback::ScopeEpoch{current_epoch + epoch_delta},
        .checkpoint =
            {
                .tick = mycore::time::Tick{tick},
                .value = static_cast<std::int8_t>(reader.byte()),
                .valid = !reader.flag(),
            },
        .events = authority_events(reader),
    };
}

[[nodiscard]] FuzzStimulus stimulus(Reader& reader) {
    return {
        .delta = static_cast<std::int8_t>(static_cast<int>(reader.byte() % 7U) - 3),
        .event_type = static_cast<std::uint8_t>(reader.byte() % 3U),
        .occurrence = static_cast<std::uint8_t>(reader.byte() % 16U),
        .fail = (reader.byte() % 13U) == 0U,
    };
}

[[nodiscard]] mycore::rollback::CommandSequence sequence(Reader& reader, const Timeline& timeline) {
    switch (reader.byte() % 4U) {
    case 0: {
        const auto base = timeline.last_submitted_sequence()
                              ? timeline.last_submitted_sequence()->value()
                              : std::uint64_t{};
        return mycore::rollback::CommandSequence{base + 1U};
    }
    case 1:
        return mycore::rollback::CommandSequence::invalid();
    case 2:
        return timeline.last_submitted_sequence().value_or(mycore::rollback::CommandSequence{0});
    default:
        return mycore::rollback::CommandSequence{reader.byte() % 32U};
    }
}

struct Context {
    Timeline timeline{FuzzModel{}, {.capacity = 16}};
    Router router{PulseHandler{}, NoticeHandler{}};
};

void apply(Context& context, CommitResult result, const TimelineSummary& before) {
    if (const auto* commit = std::get_if<Commit>(&result)) {
        check_commit(context.timeline, *commit);
        const auto report = context.router.consume(*commit);
        require(report.contract_failures.empty());
    } else {
        const auto after = summarize(context.timeline);
        require(after.statistics.failure_count == before.statistics.failure_count + 1U);
        auto expected = before;
        expected.statistics.failure_count = after.statistics.failure_count;
        require(same_summary(after, expected));
    }
    check_timeline_invariants(context.timeline);
}

void consume_event_batch(Context& context, Reader& reader, bool malformed) {
    const auto pulse = FuzzEvent{PulseEvent{
        .occurrence = static_cast<std::uint8_t>(reader.byte() % 16U),
        .payload = reader.byte(),
    }};
    auto current = pulse;
    if (malformed && reader.flag()) {
        current = NoticeEvent{.occurrence = std::get<PulseEvent>(pulse).occurrence,
                              .payload = reader.byte()};
    }
    const auto transition = static_cast<mycore::rollback::EventTransition>(reader.byte() % 5U);
    auto previous_value = std::optional<FuzzEvent>{pulse};
    auto current_value = std::optional<FuzzEvent>{current};
    if (!malformed) {
        switch (transition) {
        case mycore::rollback::EventTransition::FirstPredicted:
        case mycore::rollback::EventTransition::AuthorityOnly:
            previous_value.reset();
            break;
        case mycore::rollback::EventTransition::Revised:
        case mycore::rollback::EventTransition::Confirmed:
            break;
        case mycore::rollback::EventTransition::Retracted:
            current_value.reset();
            break;
        }
    } else if (reader.flag()) {
        previous_value.reset();
        current_value.reset();
    }
    const mycore::rollback::EventBatch<FuzzModel> batch{
        .kind = mycore::rollback::CommitKind::AuthorityOnly,
        .changes = {{
            .transition = transition,
            .key = FuzzModel{}.event_key(pulse),
            .previous = previous_value,
            .current = current_value,
        }},
        .retired_keys =
            reader.flag() ? std::vector{FuzzModel{}.event_key(pulse)} : std::vector<FuzzEventKey>{},
        .externally_retired_keys =
            reader.flag() ? std::vector{FuzzModel{}.event_key(pulse)} : std::vector<FuzzEventKey>{},
    };
    const auto statistics_before = context.router.statistics();
    const auto ledger_before = context.router.ledger_statistics();
    const auto report = context.router.consume(batch);
    if (!report.contract_failures.empty()) {
        require(context.router.statistics() == statistics_before);
        require(context.router.ledger_statistics() == ledger_before);
    }
}

void execute_operation(Context& context, Reader& reader, std::uint8_t operation) {
    const auto before = summarize(context.timeline);
    switch (operation % 10U) {
    case 0: {
        const auto tick = reader.byte() % 8U;
        auto authority = AuthorityFrame{
            .tick = mycore::time::Tick{tick},
            .acknowledged_through =
                reader.flag() ? std::optional{mycore::rollback::CommandSequence{reader.byte() % 8U}}
                              : std::nullopt,
            .scope_epoch = mycore::rollback::ScopeEpoch{1U + (reader.byte() % 3U)},
            .checkpoint =
                {
                    .tick = mycore::time::Tick{tick},
                    .value = static_cast<std::int8_t>(reader.byte()),
                    .valid = !reader.flag(),
                },
            .events = authority_events(reader),
        };
        apply(context,
              context.timeline.initialize(
                  authority, FuzzScope{static_cast<std::uint8_t>(1U + (reader.byte() % 3U))}),
              before);
        return;
    }
    case 1:
        apply(context,
              context.timeline.advance(sequence(reader, context.timeline), stimulus(reader)),
              before);
        return;
    case 2:
        apply(context,
              context.timeline.reconcile(frame(reader, context.timeline, false, false)),
              before);
        return;
    case 3:
        apply(context,
              context.timeline.refresh_authority(
                  frame(reader, context.timeline, reader.flag(), false)),
              before);
        return;
    case 4: {
        auto authority = frame(reader, context.timeline, reader.flag(), false);
        apply(context,
              context.timeline.rebase_scope(
                  authority, FuzzScope{static_cast<std::uint8_t>(1U + (reader.byte() % 3U))}),
              before);
        return;
    }
    case 5: {
        auto authority = frame(reader, context.timeline, reader.flag(), true);
        apply(context,
              context.timeline.hard_resync(
                  authority, FuzzScope{static_cast<std::uint8_t>(1U + (reader.byte() % 3U))}),
              before);
        return;
    }
    case 6: {
        const auto fail_refresh = reader.flag();
        apply(context,
              context.timeline.reconcile_with_stimulus_refresh(
                  frame(reader, context.timeline, false, false),
                  [fail_refresh](mycore::rollback::CommandSequence,
                                 const FuzzStimulus& previous,
                                 const FuzzState&,
                                 const FuzzScope&) -> std::variant<FuzzStimulus, FuzzError> {
                      return fail_refresh
                                 ? std::variant<FuzzStimulus, FuzzError>{FuzzError::StepRejected}
                                 : std::variant<FuzzStimulus, FuzzError>{previous};
                  }),
              before);
        return;
    }
    case 7:
        consume_event_batch(context, reader, false);
        check_timeline_invariants(context.timeline);
        return;
    case 8:
        consume_event_batch(context, reader, true);
        check_timeline_invariants(context.timeline);
        return;
    case 9: {
        const auto key = FuzzEventKey{.type = static_cast<std::uint8_t>(1U + (reader.byte() & 1U)),
                                      .occurrence = static_cast<std::uint8_t>(reader.byte() % 16U)};
        const mycore::rollback::EventBatch<FuzzModel> retirement{
            .kind = mycore::rollback::CommitKind::AuthorityOnly,
            .retired_keys = reader.flag() ? std::vector{key} : std::vector<FuzzEventKey>{},
            .externally_retired_keys =
                reader.flag() ? std::vector{key} : std::vector<FuzzEventKey>{},
        };
        require(context.router.consume(retirement).contract_failures.empty());
        check_timeline_invariants(context.timeline);
        return;
    }
    }
}

struct FinalSummary {
    TimelineSummary timeline;
    mycore::rollback::ConsequenceDispatchStatistics router_statistics;
    mycore::rollback::ConsequenceLedgerStatistics ledger_statistics;
    std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>
        handler_counts;
};

[[nodiscard]] bool same_final(const FinalSummary& lhs, const FinalSummary& rhs) noexcept {
    return same_summary(lhs.timeline, rhs.timeline) &&
           lhs.router_statistics == rhs.router_statistics &&
           lhs.ledger_statistics == rhs.ledger_statistics &&
           lhs.handler_counts == rhs.handler_counts;
}

[[nodiscard]] FinalSummary execute(std::span<const std::uint8_t> input) {
    Context context;
    Reader reader{input};
    const auto operation_count = std::min<std::size_t>(input.size(), 256U);
    for (auto index = std::size_t{}; index < operation_count; ++index) {
        execute_operation(context, reader, reader.byte());
    }
    check_timeline_invariants(context.timeline);
    const auto& pulse = context.router.handler<PulseHandler>();
    const auto& notice = context.router.handler<NoticeHandler>();
    return {
        .timeline = summarize(context.timeline),
        .router_statistics = context.router.statistics(),
        .ledger_statistics = context.router.ledger_statistics(),
        .handler_counts =
            {pulse.predicted, pulse.revised, pulse.canceled, pulse.confirmed, notice.confirmed},
    };
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = std::span{data, size};
    const auto first = execute(input);
    const auto second = execute(input);
    require(same_final(first, second));
    return 0;
}

#if defined(MYCORE_ROLLBACK_FUZZ_STANDALONE)
int main() {
    constexpr std::array seeds{
        std::string_view{"initialize advance reconcile authority refresh"},
        std::string_view{"refresh derived stimulus failure atomic state"},
        std::string_view{"scope epoch rebase replay retained inputs"},
        std::string_view{"hard resync acknowledgement jump clears history"},
        std::string_view{"first revised confirmed retracted authority only retirement"},
        std::string_view{"malformed transition shape changed event alternative"},
    };
    for (const auto seed : seeds) {
        LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size());
    }
    auto state = std::uint32_t{0xC0FF'EEU};
    std::array<std::uint8_t, 256> generated{};
    for (auto case_index = std::size_t{}; case_index < 1'000; ++case_index) {
        for (auto& value : generated) {
            state = state * 1'664'525U + 1'013'904'223U;
            value = static_cast<std::uint8_t>(state >> 24U);
        }
        LLVMFuzzerTestOneInput(generated.data(), 1U + (case_index % generated.size()));
    }
    return 0;
}
#endif

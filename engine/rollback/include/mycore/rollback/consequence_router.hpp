#pragma once

#include "mycore/rollback/timeline.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace mycore::rollback {

struct ConsequenceDispatchFailure {
    std::size_t handler_index{};
    ConsequenceOperation operation{ConsequenceOperation::First};

    auto operator<=>(const ConsequenceDispatchFailure&) const = default;
};

struct ConsequenceDispatchReport {
    ConsequenceDispatchStatistics statistics;
    std::vector<ConsequenceDispatchFailure> failures;
};

namespace detail {

template <class Type> inline constexpr bool is_variant_v = false;

template <class... Alternatives>
inline constexpr bool is_variant_v<std::variant<Alternatives...>> = true;

template <class Candidate, class Variant> inline constexpr bool variant_contains_v = false;

template <class Candidate, class... Alternatives>
inline constexpr bool variant_contains_v<Candidate, std::variant<Alternatives...>> =
    (std::same_as<Candidate, Alternatives> || ...);

template <class... Types> inline constexpr bool are_unique_v = true;

template <class First, class... Rest>
inline constexpr bool are_unique_v<First, Rest...> =
    (!std::same_as<First, Rest> && ...) && are_unique_v<Rest...>;

template <RollbackModel Model, class Handler, ConsequencePolicy Policy = Handler::policy>
struct ConsequenceStorage;

template <RollbackModel Model, class Handler>
struct ConsequenceStorage<Model, Handler, ConsequencePolicy::PredictOnce> {
    std::unordered_set<typename Model::EventKey, typename Model::EventKeyHash> delivered;
};

template <RollbackModel Model, class Handler>
struct ConsequenceStorage<Model, Handler, ConsequencePolicy::ConfirmOnce> {
    std::unordered_set<typename Model::EventKey, typename Model::EventKeyHash> delivered;
};

template <RollbackModel Model, class Handler>
struct ConsequenceStorage<Model, Handler, ConsequencePolicy::PredictCancelable> {
    std::unordered_map<typename Model::EventKey,
                       std::optional<typename Handler::Token>,
                       typename Model::EventKeyHash>
        active;
};

} // namespace detail

// Handlers are statically typed subscriptions. Each declares Event and policy. PredictOnce
// handlers implement bool on_first(key, event); ConfirmOnce handlers implement
// bool on_confirmed(key, event). PredictCancelable handlers declare Token and implement
// optional<Token> on_predict plus bool on_revise/on_cancel/on_confirm.
template <RollbackModel Model, class... Handlers> class StaticConsequenceRouter {
    static_assert(sizeof...(Handlers) > 0,
                  "StaticConsequenceRouter requires at least one consequence handler");
    static_assert(detail::is_variant_v<typename Model::Event>,
                  "StaticConsequenceRouter requires Model::Event to be std::variant");
    static_assert(detail::are_unique_v<Handlers...>,
                  "StaticConsequenceRouter handler types must be unique");
    static_assert((detail::variant_contains_v<typename Handlers::Event, typename Model::Event> &&
                   ...),
                  "Each consequence handler Event must be an alternative in Model::Event");

public:
    explicit StaticConsequenceRouter(Handlers... handlers)
        : handlers_(std::move(handlers)...) {}

    [[nodiscard]] ConsequenceDispatchReport consume(const Commit<Model>& commit) {
        return consume(event_batch_from_commit(commit));
    }

    [[nodiscard]] ConsequenceDispatchReport consume(const EventBatch<Model>& batch) {
        ConsequenceDispatchReport report;
        report.failures.reserve(batch.changes.size());

        for (const auto& change : batch.changes) {
            process_change(change, report, std::index_sequence_for<Handlers...>{});
        }
        for (const auto& key : batch.retired_keys) {
            retire(key, std::index_sequence_for<Handlers...>{});
        }

        add_statistics(statistics_, report.statistics);
        return report;
    }

    template <class Handler> [[nodiscard]] Handler& handler() noexcept {
        return std::get<Handler>(handlers_);
    }

    template <class Handler> [[nodiscard]] const Handler& handler() const noexcept {
        return std::get<Handler>(handlers_);
    }

    [[nodiscard]] const ConsequenceDispatchStatistics& statistics() const noexcept {
        return statistics_;
    }

private:
    static void add_statistics(ConsequenceDispatchStatistics& destination,
                               const ConsequenceDispatchStatistics& source) noexcept {
        destination.delivered_count += source.delivered_count;
        destination.suppressed_count += source.suppressed_count;
        destination.revised_count += source.revised_count;
        destination.canceled_count += source.canceled_count;
        destination.confirmed_count += source.confirmed_count;
        destination.failure_count += source.failure_count;
    }

    template <std::size_t... Indices>
    void process_change(const EventChange<Model>& change,
                        ConsequenceDispatchReport& report,
                        std::index_sequence<Indices...>) {
        (process_handler<Indices>(change, report), ...);
    }

    template <std::size_t Index>
    void process_handler(const EventChange<Model>& change, ConsequenceDispatchReport& report) {
        using Handler = std::tuple_element_t<Index, std::tuple<Handlers...>>;
        using Event = typename Handler::Event;

        const auto* current = typed_event<Event>(change.current);
        const auto* previous = typed_event<Event>(change.previous);
        if (current == nullptr && previous == nullptr) {
            return;
        }

        if constexpr (Handler::policy == ConsequencePolicy::PredictOnce) {
            process_predict_once<Index>(change, current != nullptr ? *current : *previous, report);
        } else if constexpr (Handler::policy == ConsequencePolicy::PredictCancelable) {
            process_predict_cancelable<Index>(change, current, previous, report);
        } else {
            static_assert(Handler::policy == ConsequencePolicy::ConfirmOnce);
            process_confirm_once<Index>(change, current != nullptr ? *current : *previous, report);
        }
    }

    template <class Event>
    [[nodiscard]] static const Event*
    typed_event(const std::optional<typename Model::Event>& event) noexcept {
        if (!event) {
            return nullptr;
        }
        return std::get_if<Event>(&*event);
    }

    template <std::size_t Index, class Event>
    void process_predict_once(const EventChange<Model>& change,
                              const Event& event,
                              ConsequenceDispatchReport& report) {
        if (change.transition != EventTransition::FirstPredicted &&
            change.transition != EventTransition::Confirmed &&
            change.transition != EventTransition::AuthorityOnly) {
            ++report.statistics.suppressed_count;
            return;
        }

        auto& storage = std::get<Index>(storage_).delivered;
        if (!storage.insert(change.key).second) {
            ++report.statistics.suppressed_count;
            return;
        }

        auto& handler = std::get<Index>(handlers_);
        if (!handler.on_first(change.key, event)) {
            record_failure<Index>(ConsequenceOperation::First, report);
            return;
        }
        ++report.statistics.delivered_count;
    }

    template <std::size_t Index, class Event>
    void process_confirm_once(const EventChange<Model>& change,
                              const Event& event,
                              ConsequenceDispatchReport& report) {
        if (change.transition != EventTransition::Confirmed &&
            change.transition != EventTransition::AuthorityOnly) {
            ++report.statistics.suppressed_count;
            return;
        }

        auto& storage = std::get<Index>(storage_).delivered;
        if (!storage.insert(change.key).second) {
            ++report.statistics.suppressed_count;
            return;
        }

        auto& handler = std::get<Index>(handlers_);
        if (!handler.on_confirmed(change.key, event)) {
            record_failure<Index>(ConsequenceOperation::Confirmed, report);
            return;
        }
        ++report.statistics.delivered_count;
        ++report.statistics.confirmed_count;
    }

    template <std::size_t Index, class Event>
    void process_predict_cancelable(const EventChange<Model>& change,
                                    const Event* current,
                                    const Event* previous,
                                    ConsequenceDispatchReport& report) {
        auto& handler = std::get<Index>(handlers_);
        auto& active = std::get<Index>(storage_).active;
        auto found = active.find(change.key);

        switch (change.transition) {
        case EventTransition::FirstPredicted:
            if (found != active.end()) {
                ++report.statistics.suppressed_count;
                return;
            }
            activate<Index>(change.key, *current, report);
            return;
        case EventTransition::Revised:
            if (found == active.end() || !found->second) {
                ++report.statistics.suppressed_count;
                return;
            }
            if (!handler.on_revise(*found->second, change.key, *current)) {
                record_failure<Index>(ConsequenceOperation::Revise, report);
                return;
            }
            ++report.statistics.revised_count;
            return;
        case EventTransition::Retracted:
            if (found == active.end()) {
                ++report.statistics.suppressed_count;
                return;
            }
            if (found->second && !handler.on_cancel(*found->second, change.key, *previous)) {
                record_failure<Index>(ConsequenceOperation::Cancel, report);
            } else if (found->second) {
                ++report.statistics.canceled_count;
            }
            active.erase(found);
            return;
        case EventTransition::Confirmed: {
            if (found == active.end()) {
                activate<Index>(change.key, *current, report);
                found = active.find(change.key);
            }
            if (found == active.end() || !found->second) {
                ++report.statistics.suppressed_count;
                return;
            }
            const auto succeeded = handler.on_confirm(*found->second, change.key, *current);
            found->second.reset();
            if (!succeeded) {
                record_failure<Index>(ConsequenceOperation::Confirm, report);
                return;
            }
            ++report.statistics.confirmed_count;
            return;
        }
        case EventTransition::AuthorityOnly: {
            if (found == active.end()) {
                activate<Index>(change.key, *current, report);
                found = active.find(change.key);
            }
            if (found == active.end() || !found->second) {
                ++report.statistics.suppressed_count;
                return;
            }
            const auto succeeded = handler.on_confirm(*found->second, change.key, *current);
            found->second.reset();
            if (!succeeded) {
                record_failure<Index>(ConsequenceOperation::Confirm, report);
                return;
            }
            ++report.statistics.confirmed_count;
            return;
        }
        }
    }

    template <std::size_t Index, class Event>
    void activate(const typename Model::EventKey& key,
                  const Event& event,
                  ConsequenceDispatchReport& report) {
        auto& handler = std::get<Index>(handlers_);
        auto& active = std::get<Index>(storage_).active;
        auto [found, inserted] = active.emplace(key, std::nullopt);
        if (!inserted) {
            ++report.statistics.suppressed_count;
            return;
        }

        auto token = handler.on_predict(key, event);
        if (!token) {
            record_failure<Index>(ConsequenceOperation::Predict, report);
            return;
        }
        found->second.emplace(std::move(*token));
        ++report.statistics.delivered_count;
    }

    template <std::size_t Index>
    static void record_failure(ConsequenceOperation operation, ConsequenceDispatchReport& report) {
        ++report.statistics.failure_count;
        report.failures.push_back(
            ConsequenceDispatchFailure{.handler_index = Index, .operation = operation});
    }

    template <std::size_t... Indices>
    void retire(const typename Model::EventKey& key, std::index_sequence<Indices...>) {
        (retire_handler<Indices>(key), ...);
    }

    template <std::size_t Index> void retire_handler(const typename Model::EventKey& /*key*/) {
        using Handler = std::tuple_element_t<Index, std::tuple<Handlers...>>;
        static_assert(Handler::policy == ConsequencePolicy::PredictOnce ||
                      Handler::policy == ConsequencePolicy::PredictCancelable ||
                      Handler::policy == ConsequencePolicy::ConfirmOnce);
        // Keep tombstones for the session until the game adapter can prove its authority receipt
        // watermark has retired the key. Retracted cancelable entries are erased when canceled,
        // which intentionally permits a real later reactivation.
    }

    std::tuple<Handlers...> handlers_;
    std::tuple<detail::ConsequenceStorage<Model, Handlers>...> storage_;
    ConsequenceDispatchStatistics statistics_;
};

} // namespace mycore::rollback

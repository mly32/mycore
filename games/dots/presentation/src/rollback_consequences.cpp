#include "dots/presentation/rollback_consequences.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace dots::presentation {
namespace {

enum class CueType : std::uint8_t {
    SplitFlash,
    SplitLaunch,
    FoodPop,
    ConsumeFlash,
    ConsumeCollapse,
    ConfirmedAbsorption,
};

struct CueToken {
    std::uint64_t value{};
};

struct CueRecord {
    std::uint64_t token{};
    CueType type{CueType::SplitFlash};
    protocol::EntityId attached_entity_id;
    mycore::math::Vector2 position;
    mycore::math::Vector2 target;
    mycore::math::Vector2 velocity;
    float mass{1.0F};
    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point canceled_at;
    std::chrono::steady_clock::duration duration{};
    std::chrono::steady_clock::duration cancellation_duration{};
    bool one_shot{};
    bool canceled{};
    bool terminal{};
};

[[nodiscard]] float progress(std::chrono::steady_clock::time_point now,
                             std::chrono::steady_clock::time_point started_at,
                             std::chrono::steady_clock::duration duration) noexcept {
    if (duration <= std::chrono::steady_clock::duration::zero()) {
        return 1.0F;
    }
    const auto age = std::max(now - started_at, std::chrono::steady_clock::duration::zero());
    return std::clamp(std::chrono::duration<float>{age}.count() /
                          std::chrono::duration<float>{duration}.count(),
                      0.0F,
                      1.0F);
}

class CueStore {
public:
    void set_observed_at(std::chrono::steady_clock::time_point observed_at) noexcept {
        observed_at_ = observed_at;
    }

    void set_local_owner(simulation::PlayerOwnerId owner_id) noexcept {
        local_owner_id_ = owner_id;
    }

    void emit(CueType type,
              mycore::math::Vector2 position,
              mycore::math::Vector2 target,
              mycore::math::Vector2 velocity,
              float mass,
              std::chrono::steady_clock::duration duration) {
        records_.push_back({
            .token = next_token_++,
            .type = type,
            .attached_entity_id = {},
            .position = position,
            .target = target,
            .velocity = velocity,
            .mass = mass,
            .started_at = observed_at_,
            .canceled_at = {},
            .duration = duration,
            .cancellation_duration = {},
            .one_shot = true,
            .canceled = false,
            .terminal = true,
        });
    }

    [[nodiscard]] CueToken activate(CueType type,
                                    protocol::EntityId attached_entity_id,
                                    mycore::math::Vector2 position,
                                    mycore::math::Vector2 target,
                                    mycore::math::Vector2 velocity,
                                    float mass,
                                    std::chrono::steady_clock::duration duration,
                                    std::chrono::steady_clock::duration cancellation_duration) {
        const auto token = next_token_++;
        records_.push_back({
            .token = token,
            .type = type,
            .attached_entity_id = attached_entity_id,
            .position = position,
            .target = target,
            .velocity = velocity,
            .mass = mass,
            .started_at = observed_at_,
            .canceled_at = {},
            .duration = duration,
            .cancellation_duration = cancellation_duration,
            .one_shot = false,
            .canceled = false,
            .terminal = false,
        });
        return CueToken{.value = token};
    }

    template <class Update> void revise(CueToken token, Update&& update) {
        if (auto* record = find(token)) {
            std::forward<Update>(update)(*record);
        }
    }

    void cancel(CueToken token) noexcept {
        if (auto* record = find(token)) {
            record->canceled = true;
            record->terminal = true;
            record->canceled_at = observed_at_;
        }
    }

    void confirm(CueToken token) noexcept {
        if (auto* record = find(token)) {
            record->terminal = true;
        }
    }

    void confirm_absorption(const simulation::PlayerAbsorbed& event) {
        emit(CueType::ConfirmedAbsorption,
             event.victim_position,
             event.absorber_position,
             {},
             event.transferred_mass,
             kConfirmedNoticeDuration);
        auto kind = ConfirmedNoticeKind::Absorption;
        if (local_owner_id_.is_valid()) {
            if (event.victim_owner_id == local_owner_id_) {
                kind = ConfirmedNoticeKind::Defeat;
            } else if (event.absorber_owner_id == local_owner_id_) {
                kind = ConfirmedNoticeKind::Kill;
            }
        }
        ++stinger_sequence_;
        notice_ = NoticeRecord{
            .kind = kind,
            .sequence = stinger_sequence_,
            .started_at = observed_at_,
        };
    }

    void append(FrameData& frame, std::chrono::steady_clock::time_point now) {
        visible_count_ = 0;
        for (auto& record : records_) {
            const auto cue_progress = progress(now, record.started_at, record.duration);
            auto cancellation_opacity = 1.0F;
            if (record.canceled) {
                cancellation_opacity =
                    1.0F - progress(now, record.canceled_at, record.cancellation_duration);
            }
            if (cue_progress >= 1.0F || cancellation_opacity <= 0.0F) {
                continue;
            }
            ++visible_count_;
            append_record(frame, record, cue_progress, cancellation_opacity);
        }
        std::erase_if(records_, [now](const CueRecord& record) {
            const auto expired = progress(now, record.started_at, record.duration) >= 1.0F;
            const auto cancellation_finished =
                record.canceled &&
                progress(now, record.canceled_at, record.cancellation_duration) >= 1.0F;
            return record.one_shot ? expired
                                   : record.terminal && (expired || cancellation_finished);
        });
    }

    [[nodiscard]] std::optional<ConfirmedNotice>
    notice(std::chrono::steady_clock::time_point now) const noexcept {
        if (!notice_) {
            return std::nullopt;
        }
        const auto notice_progress = progress(now, notice_->started_at, kConfirmedNoticeDuration);
        if (notice_progress >= 1.0F) {
            return std::nullopt;
        }
        return ConfirmedNotice{
            .kind = notice_->kind,
            .sequence = notice_->sequence,
            .opacity = 1.0F - notice_progress,
        };
    }

    [[nodiscard]] std::size_t visible_count() const noexcept {
        return visible_count_;
    }

    [[nodiscard]] std::uint64_t stinger_sequence() const noexcept {
        return stinger_sequence_;
    }

private:
    struct NoticeRecord {
        ConfirmedNoticeKind kind{ConfirmedNoticeKind::Absorption};
        std::uint64_t sequence{};
        std::chrono::steady_clock::time_point started_at;
    };

    [[nodiscard]] CueRecord* find(CueToken token) noexcept {
        const auto found = std::ranges::find(records_, token.value, &CueRecord::token);
        return found == records_.end() ? nullptr : &*found;
    }

    static void append_circle(FrameData& frame,
                              mycore::math::Vector2 position,
                              float mass,
                              float radius,
                              CircleKind kind,
                              protocol::EntityId entity_id,
                              float opacity) {
        frame.circles.push_back({
            .position = position,
            .mass = mass,
            .radius = radius,
            .kind = kind,
            .entity_id = entity_id,
            .owner_id = {},
            .opacity = opacity,
            .prediction_key = std::nullopt,
            .source = PresentationSource::State,
            .source_revision = 0,
        });
    }

    static void append_record(FrameData& frame,
                              const CueRecord& record,
                              float cue_progress,
                              float cancellation_opacity) {
        const auto base_radius = simulation::radius_for_mass(std::max(record.mass, 0.01F));
        const auto fade = (1.0F - cue_progress) * cancellation_opacity;
        auto attached_position = record.position;
        if (record.attached_entity_id.is_valid()) {
            const auto attached =
                std::ranges::find_if(frame.circles, [&record](const CircleInstance& circle) {
                    return circle.kind == CircleKind::Player &&
                           circle.entity_id == record.attached_entity_id;
                });
            if (attached != frame.circles.end()) {
                attached_position = attached->position;
            } else {
                const auto age =
                    std::chrono::duration<float>{record.duration}.count() * cue_progress;
                attached_position += record.velocity * age;
            }
        }

        switch (record.type) {
        case CueType::SplitFlash:
            append_circle(frame,
                          record.position,
                          record.mass,
                          base_radius * (0.8F + cue_progress),
                          CircleKind::SplitFlash,
                          {},
                          fade);
            return;
        case CueType::SplitLaunch:
            append_circle(frame,
                          attached_position,
                          record.mass,
                          base_radius * (1.15F + (0.25F * cue_progress)),
                          CircleKind::SplitLaunch,
                          record.attached_entity_id,
                          fade);
            return;
        case CueType::FoodPop: {
            constexpr std::array directions{
                mycore::math::Vector2{1.0F, 0.0F},
                mycore::math::Vector2{-1.0F, 0.0F},
                mycore::math::Vector2{0.0F, 1.0F},
                mycore::math::Vector2{0.0F, -1.0F},
            };
            for (const auto direction : directions) {
                append_circle(frame,
                              record.position + (direction * base_radius * (0.25F + cue_progress)),
                              record.mass,
                              std::max(base_radius * 0.16F * (1.0F - cue_progress), 0.03F),
                              CircleKind::FoodPop,
                              {},
                              cancellation_opacity);
            }
            return;
        }
        case CueType::ConsumeFlash:
            append_circle(frame,
                          record.position,
                          record.mass,
                          base_radius * (1.0F + (0.6F * cue_progress)),
                          CircleKind::ConsumeFlash,
                          {},
                          fade);
            return;
        case CueType::ConsumeCollapse:
            append_circle(frame,
                          record.position + ((record.target - record.position) * cue_progress),
                          record.mass,
                          base_radius * (1.0F - cue_progress),
                          CircleKind::ConsumeCollapse,
                          {},
                          cancellation_opacity);
            return;
        case CueType::ConfirmedAbsorption:
            append_circle(frame,
                          record.position,
                          record.mass,
                          base_radius * (1.0F + cue_progress),
                          CircleKind::ConfirmedAbsorption,
                          {},
                          fade);
            return;
        }
    }

    std::vector<CueRecord> records_;
    std::optional<NoticeRecord> notice_;
    simulation::PlayerOwnerId local_owner_id_;
    std::chrono::steady_clock::time_point observed_at_;
    std::uint64_t next_token_{1};
    std::uint64_t stinger_sequence_{};
    std::size_t visible_count_{};
};

struct SplitFlashHandler {
    using Event = simulation::PlayerSplit;
    static constexpr auto policy = mycore::rollback::ConsequencePolicy::PredictOnce;

    [[nodiscard]] bool on_first(const simulation::SimulationEventKey&, const Event& event) {
        store->emit(CueType::SplitFlash,
                    event.origin_position,
                    {},
                    event.initial_launch_velocity,
                    event.child_mass,
                    kSplitFlashDuration);
        return true;
    }

    CueStore* store{};
};

struct SplitLaunchHandler {
    using Event = simulation::PlayerSplit;
    using Token = CueToken;
    static constexpr auto policy = mycore::rollback::ConsequencePolicy::PredictCancelable;

    [[nodiscard]] std::optional<Token> on_predict(const simulation::SimulationEventKey&,
                                                  const Event& event) {
        return store->activate(CueType::SplitLaunch,
                               protocol::EntityId{event.child_entity_id.value()},
                               event.origin_position,
                               {},
                               event.initial_launch_velocity,
                               event.child_mass,
                               kSplitLaunchCueDuration,
                               kCancelableCueCancellationDuration);
    }

    [[nodiscard]] bool
    on_revise(Token& token, const simulation::SimulationEventKey&, const Event& event) {
        store->revise(token, [&event](CueRecord& record) {
            record.attached_entity_id = protocol::EntityId{event.child_entity_id.value()};
            record.position = event.origin_position;
            record.velocity = event.initial_launch_velocity;
            record.mass = event.child_mass;
        });
        return true;
    }

    [[nodiscard]] bool
    on_cancel(Token& token, const simulation::SimulationEventKey&, const Event&) {
        store->cancel(token);
        return true;
    }

    [[nodiscard]] bool
    on_confirm(Token& token, const simulation::SimulationEventKey&, const Event& event) {
        static_cast<void>(on_revise(token, {}, event));
        store->confirm(token);
        return true;
    }

    CueStore* store{};
};

struct FoodPopHandler {
    using Event = simulation::FoodConsumed;
    using Token = CueToken;
    static constexpr auto policy = mycore::rollback::ConsequencePolicy::PredictCancelable;

    [[nodiscard]] std::optional<Token> on_predict(const simulation::SimulationEventKey&,
                                                  const Event& event) {
        return store->activate(CueType::FoodPop,
                               {},
                               event.food_position,
                               {},
                               {},
                               event.transferred_mass,
                               kFoodPopDuration,
                               kFoodPopCancellationDuration);
    }

    [[nodiscard]] bool
    on_revise(Token& token, const simulation::SimulationEventKey&, const Event& event) {
        store->revise(token, [&event](CueRecord& record) {
            record.position = event.food_position;
            record.mass = event.transferred_mass;
        });
        return true;
    }

    [[nodiscard]] bool
    on_cancel(Token& token, const simulation::SimulationEventKey&, const Event&) {
        store->cancel(token);
        return true;
    }

    [[nodiscard]] bool
    on_confirm(Token& token, const simulation::SimulationEventKey&, const Event& event) {
        static_cast<void>(on_revise(token, {}, event));
        store->confirm(token);
        return true;
    }

    CueStore* store{};
};

struct ConsumeFlashHandler {
    using Event = simulation::PlayerAbsorbed;
    static constexpr auto policy = mycore::rollback::ConsequencePolicy::PredictOnce;

    [[nodiscard]] bool on_first(const simulation::SimulationEventKey&, const Event& event) {
        store->emit(CueType::ConsumeFlash,
                    event.victim_position,
                    event.absorber_position,
                    {},
                    event.transferred_mass,
                    kConsumeFlashDuration);
        return true;
    }

    CueStore* store{};
};

struct ConsumeCollapseHandler {
    using Event = simulation::PlayerAbsorbed;
    using Token = CueToken;
    static constexpr auto policy = mycore::rollback::ConsequencePolicy::PredictCancelable;

    [[nodiscard]] std::optional<Token> on_predict(const simulation::SimulationEventKey&,
                                                  const Event& event) {
        return store->activate(CueType::ConsumeCollapse,
                               {},
                               event.victim_position,
                               event.absorber_position,
                               {},
                               event.transferred_mass,
                               kConsumeCollapseDuration,
                               kCancelableCueCancellationDuration);
    }

    [[nodiscard]] bool
    on_revise(Token& token, const simulation::SimulationEventKey&, const Event& event) {
        store->revise(token, [&event](CueRecord& record) {
            record.position = event.victim_position;
            record.target = event.absorber_position;
            record.mass = event.transferred_mass;
        });
        return true;
    }

    [[nodiscard]] bool
    on_cancel(Token& token, const simulation::SimulationEventKey&, const Event&) {
        store->cancel(token);
        return true;
    }

    [[nodiscard]] bool
    on_confirm(Token& token, const simulation::SimulationEventKey&, const Event& event) {
        static_cast<void>(on_revise(token, {}, event));
        store->confirm(token);
        return true;
    }

    CueStore* store{};
};

struct ConfirmedAbsorptionHandler {
    using Event = simulation::PlayerAbsorbed;
    static constexpr auto policy = mycore::rollback::ConsequencePolicy::ConfirmOnce;

    [[nodiscard]] bool on_confirmed(const simulation::SimulationEventKey&, const Event& event) {
        store->confirm_absorption(event);
        return true;
    }

    CueStore* store{};
};

using Router = mycore::rollback::StaticConsequenceRouter<prediction::WorldModel,
                                                         SplitFlashHandler,
                                                         SplitLaunchHandler,
                                                         FoodPopHandler,
                                                         ConsumeFlashHandler,
                                                         ConsumeCollapseHandler,
                                                         ConfirmedAbsorptionHandler>;

} // namespace

class RollbackConsequencePresentation::Impl {
public:
    Impl()
        : router_(SplitFlashHandler{.store = &store_},
                  SplitLaunchHandler{.store = &store_},
                  FoodPopHandler{.store = &store_},
                  ConsumeFlashHandler{.store = &store_},
                  ConsumeCollapseHandler{.store = &store_},
                  ConfirmedAbsorptionHandler{.store = &store_}) {}

    void set_local_owner(simulation::PlayerOwnerId owner_id) noexcept {
        store_.set_local_owner(owner_id);
    }

    [[nodiscard]] mycore::rollback::ConsequenceDispatchReport
    consume(const PredictionEventBatch& batch, std::chrono::steady_clock::time_point observed_at) {
        store_.set_observed_at(observed_at);
        for (const auto& change : batch.changes) {
            ++statistics_.transition_counts[static_cast<std::size_t>(change.transition)];
        }
        auto report = router_.consume(batch);
        ++statistics_.consumed_batch_count;
        statistics_.dispatch = router_.statistics();
        statistics_.handlers.assign(router_.handler_statistics().begin(),
                                    router_.handler_statistics().end());
        statistics_.handler_failure_count += report.failures.size();
        statistics_.stinger_sequence = store_.stinger_sequence();
        return report;
    }

    void append_cues(FrameData& frame, std::chrono::steady_clock::time_point now) {
        store_.append(frame, now);
        statistics_.visible_cue_count = store_.visible_count();
        statistics_.stinger_sequence = store_.stinger_sequence();
    }

    [[nodiscard]] std::optional<ConfirmedNotice>
    confirmed_notice(std::chrono::steady_clock::time_point now) const noexcept {
        return store_.notice(now);
    }

    [[nodiscard]] const ConsequencePresentationStatistics& statistics() const noexcept {
        return statistics_;
    }

private:
    CueStore store_;
    Router router_;
    ConsequencePresentationStatistics statistics_;
};

RollbackConsequencePresentation::RollbackConsequencePresentation()
    : impl_(std::make_unique<Impl>()) {}

RollbackConsequencePresentation::~RollbackConsequencePresentation() = default;
RollbackConsequencePresentation::RollbackConsequencePresentation(
    RollbackConsequencePresentation&&) noexcept = default;
RollbackConsequencePresentation&
RollbackConsequencePresentation::operator=(RollbackConsequencePresentation&&) noexcept = default;

void RollbackConsequencePresentation::set_local_owner(simulation::PlayerOwnerId owner_id) noexcept {
    impl_->set_local_owner(owner_id);
}

mycore::rollback::ConsequenceDispatchReport
RollbackConsequencePresentation::consume(const PredictionEventBatch& batch,
                                         std::chrono::steady_clock::time_point observed_at) {
    return impl_->consume(batch, observed_at);
}

void RollbackConsequencePresentation::append_cues(FrameData& frame,
                                                  std::chrono::steady_clock::time_point now) {
    impl_->append_cues(frame, now);
}

std::optional<ConfirmedNotice> RollbackConsequencePresentation::confirmed_notice(
    std::chrono::steady_clock::time_point now) const noexcept {
    return impl_->confirmed_notice(now);
}

const ConsequencePresentationStatistics&
RollbackConsequencePresentation::statistics() const noexcept {
    return impl_->statistics();
}

std::string_view confirmed_notice_text(ConfirmedNoticeKind kind) noexcept {
    switch (kind) {
    case ConfirmedNoticeKind::Kill:
        return "PLAYER CONSUMED";
    case ConfirmedNoticeKind::Defeat:
        return "DEFEATED";
    case ConfirmedNoticeKind::Absorption:
        return "ABSORPTION CONFIRMED";
    }
    return "ABSORPTION CONFIRMED";
}

} // namespace dots::presentation

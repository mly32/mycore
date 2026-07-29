#pragma once

#include "dots/prediction/model.hpp"
#include "dots/presentation/presentation.hpp"
#include "dots/simulation/ids.hpp"
#include "mycore/rollback/rollback.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace dots::presentation {

inline constexpr auto kSplitFlashDuration = std::chrono::milliseconds{180};
inline constexpr auto kSplitLaunchCueDuration = std::chrono::seconds{1};
inline constexpr auto kFoodPopDuration = std::chrono::milliseconds{250};
inline constexpr auto kFoodPopCancellationDuration = std::chrono::milliseconds{80};
inline constexpr auto kConsumeFlashDuration = std::chrono::milliseconds{150};
inline constexpr auto kConsumeCollapseDuration = std::chrono::milliseconds{300};
inline constexpr auto kCancelableCueCancellationDuration = std::chrono::milliseconds{100};
inline constexpr auto kConfirmedNoticeDuration = std::chrono::milliseconds{1500};

enum class ConfirmedNoticeKind : std::uint8_t {
    Kill,
    Defeat,
    Absorption,
};

struct ConfirmedNotice {
    ConfirmedNoticeKind kind{ConfirmedNoticeKind::Absorption};
    std::uint64_t sequence{};
    float opacity{1.0F};

    bool operator==(const ConfirmedNotice&) const = default;
};

struct ConsequencePresentationStatistics {
    mycore::rollback::ConsequenceDispatchStatistics dispatch;
    std::vector<mycore::rollback::ConsequenceHandlerDispatchStatistics> handlers;
    std::array<std::uint64_t, 5> transition_counts{};
    std::size_t visible_cue_count{};
    std::uint64_t consumed_batch_count{};
    std::uint64_t stinger_sequence{};
    std::uint64_t handler_failure_count{};
};

using PredictionEventBatch = mycore::rollback::EventBatch<prediction::WorldModel>;

// Dots-owned production example of the generic consequence policies. Simulation and the
// rollback timeline remain side-effect free; this object owns only presentation tokens and a
// non-rewindable router ledger.
class RollbackConsequencePresentation {
public:
    RollbackConsequencePresentation();
    ~RollbackConsequencePresentation();

    RollbackConsequencePresentation(const RollbackConsequencePresentation&) = delete;
    RollbackConsequencePresentation&
    operator=(const RollbackConsequencePresentation&) = delete;
    RollbackConsequencePresentation(RollbackConsequencePresentation&&) noexcept;
    RollbackConsequencePresentation&
    operator=(RollbackConsequencePresentation&&) noexcept;

    void set_local_owner(simulation::PlayerOwnerId owner_id) noexcept;
    [[nodiscard]] mycore::rollback::ConsequenceDispatchReport
    consume(const PredictionEventBatch& batch,
            std::chrono::steady_clock::time_point observed_at);
    void append_cues(FrameData& frame, std::chrono::steady_clock::time_point now);

    [[nodiscard]] std::optional<ConfirmedNotice>
    confirmed_notice(std::chrono::steady_clock::time_point now) const noexcept;
    [[nodiscard]] const ConsequencePresentationStatistics& statistics() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view confirmed_notice_text(ConfirmedNoticeKind kind) noexcept;

} // namespace dots::presentation

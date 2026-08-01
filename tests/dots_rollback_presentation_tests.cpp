#include "dots/presentation/rollback_consequences.hpp"

#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] constexpr std::chrono::steady_clock::time_point
clock_time(std::chrono::steady_clock::duration offset) noexcept {
    return std::chrono::steady_clock::time_point{offset};
}

[[nodiscard]] dots::presentation::PredictionEventBatch
batch(mycore::rollback::EventTransition transition,
      dots::simulation::SimulationEvent current,
      std::optional<dots::simulation::SimulationEvent> previous = std::nullopt) {
    const auto key = dots::simulation::simulation_event_key(current);
    return {
        .changes = {{
            .transition = transition,
            .key = key,
            .previous = previous,
            .current = transition == mycore::rollback::EventTransition::Retracted
                           ? std::nullopt
                           : std::optional{current},
        }},
    };
}

[[nodiscard]] dots::simulation::PlayerSplit split(float origin_x = 1.0F) {
    return {
        .tick = mycore::time::Tick{4},
        .owner_id = dots::simulation::PlayerOwnerId{2},
        .input_id = dots::simulation::InputCommandId{9},
        .child_ordinal = 1,
        .parent_entity_id = dots::simulation::EntityId{10},
        .child_entity_id = dots::simulation::EntityId{11},
        .origin_position = {origin_x, 2.0F},
        .initial_launch_velocity = {3.0F, 0.0F},
        .parent_mass = 8.0F,
        .child_mass = 8.0F,
    };
}

[[nodiscard]] dots::simulation::PlayerAbsorbed
absorption(dots::simulation::EntityId victim_id = dots::simulation::EntityId{20}) {
    return {
        .tick = mycore::time::Tick{7},
        .absorber_entity_id = dots::simulation::EntityId{10},
        .victim_entity_id = victim_id,
        .absorber_owner_id = dots::simulation::PlayerOwnerId{2},
        .victim_owner_id = dots::simulation::PlayerOwnerId{3},
        .absorber_position = {4.0F, 5.0F},
        .victim_position = {6.0F, 5.0F},
        .transferred_mass = 4.0F,
    };
}

[[nodiscard]] std::size_t circle_count(const dots::presentation::FrameData& frame,
                                       dots::presentation::CircleKind kind) {
    return static_cast<std::size_t>(
        std::ranges::count(frame.circles, kind, &dots::presentation::CircleInstance::kind));
}

} // namespace

TEST_CASE("Dots split consequences are idempotent, revisable, and cancelable",
          "[dots][rollback][presentation]") {
    dots::presentation::RollbackConsequencePresentation presentation;
    const auto first_event = dots::simulation::SimulationEvent{split()};
    const auto first = batch(mycore::rollback::EventTransition::FirstPredicted, first_event);
    REQUIRE(presentation.consume(first, clock_time(0ms)).failures.empty());

    dots::presentation::FrameData frame;
    presentation.append_cues(frame, clock_time(10ms));
    CHECK(circle_count(frame, dots::presentation::CircleKind::SplitFlash) == 1);
    CHECK(circle_count(frame, dots::presentation::CircleKind::SplitLaunch) == 1);

    REQUIRE(presentation.consume(first, clock_time(20ms)).failures.empty());
    frame.circles.clear();
    presentation.append_cues(frame, clock_time(30ms));
    CHECK(circle_count(frame, dots::presentation::CircleKind::SplitFlash) == 1);
    CHECK(circle_count(frame, dots::presentation::CircleKind::SplitLaunch) == 1);

    const auto revised_event = dots::simulation::SimulationEvent{split(8.0F)};
    REQUIRE(
        presentation
            .consume(batch(mycore::rollback::EventTransition::Revised, revised_event, first_event),
                     clock_time(40ms))
            .failures.empty());
    const auto retracted =
        batch(mycore::rollback::EventTransition::Retracted, revised_event, revised_event);
    REQUIRE(presentation.consume(retracted, clock_time(50ms)).failures.empty());

    frame.circles.clear();
    presentation.append_cues(frame, clock_time(151ms));
    CHECK(circle_count(frame, dots::presentation::CircleKind::SplitLaunch) == 0);
    CHECK(circle_count(frame, dots::presentation::CircleKind::SplitFlash) == 1);
    CHECK(presentation.statistics().dispatch.revised_count == 1);
    CHECK(presentation.statistics().dispatch.canceled_count == 1);
    REQUIRE(presentation.statistics().handlers.size() == 6);
    CHECK(presentation.statistics().handlers[0].policy ==
          mycore::rollback::ConsequencePolicy::PredictOnce);
    CHECK(presentation.statistics().handlers[1].policy ==
          mycore::rollback::ConsequencePolicy::PredictCancelable);
}

TEST_CASE("Confirmed absorption emits exactly one irreversible notice and stinger hook",
          "[dots][rollback][presentation]") {
    dots::presentation::RollbackConsequencePresentation presentation;
    presentation.set_local_owner(dots::simulation::PlayerOwnerId{2});
    const auto event = dots::simulation::SimulationEvent{absorption()};
    const auto predicted = batch(mycore::rollback::EventTransition::FirstPredicted, event);
    REQUIRE(presentation.consume(predicted, clock_time(0ms)).failures.empty());
    CHECK_FALSE(presentation.confirmed_notice(clock_time(1ms)).has_value());

    const auto confirmed = batch(mycore::rollback::EventTransition::Confirmed, event, event);
    REQUIRE(presentation.consume(confirmed, clock_time(20ms)).failures.empty());
    const auto notice = presentation.confirmed_notice(clock_time(20ms));
    REQUIRE(notice.has_value());
    CHECK(notice->kind == dots::presentation::ConfirmedNoticeKind::Kill);
    CHECK(notice->sequence == 1);
    CHECK(dots::presentation::confirmed_notice_text(notice->kind) == "PLAYER CONSUMED");

    REQUIRE(presentation.consume(confirmed, clock_time(30ms)).failures.empty());
    CHECK(presentation.statistics().stinger_sequence == 1);

    dots::presentation::FrameData frame;
    presentation.append_cues(frame, clock_time(40ms));
    CHECK(circle_count(frame, dots::presentation::CircleKind::ConsumeFlash) == 1);
    CHECK(circle_count(frame, dots::presentation::CircleKind::ConsumeCollapse) == 1);
    CHECK(circle_count(frame, dots::presentation::CircleKind::ConfirmedAbsorption) == 1);

    const auto authority_event =
        dots::simulation::SimulationEvent{absorption(dots::simulation::EntityId{21})};
    REQUIRE(presentation
                .consume(batch(mycore::rollback::EventTransition::AuthorityOnly, authority_event),
                         clock_time(50ms))
                .failures.empty());
    CHECK(presentation.statistics().stinger_sequence == 2);
    CHECK(presentation.statistics().transition_counts[static_cast<std::size_t>(
              mycore::rollback::EventTransition::AuthorityOnly)] == 1);
}

TEST_CASE("Cancelable food pop fades quickly after rollback retraction",
          "[dots][rollback][presentation]") {
    dots::presentation::RollbackConsequencePresentation presentation;
    const auto event = dots::simulation::SimulationEvent{dots::simulation::FoodConsumed{
        .tick = mycore::time::Tick{3},
        .food_entity_id = dots::simulation::EntityId{30},
        .consumer_entity_id = dots::simulation::EntityId{10},
        .consumer_owner_id = dots::simulation::PlayerOwnerId{2},
        .food_position = {7.0F, 8.0F},
        .transferred_mass = 1.0F,
    }};
    REQUIRE(presentation
                .consume(batch(mycore::rollback::EventTransition::FirstPredicted, event),
                         clock_time(0ms))
                .failures.empty());
    REQUIRE(presentation
                .consume(batch(mycore::rollback::EventTransition::Retracted, event, event),
                         clock_time(10ms))
                .failures.empty());

    dots::presentation::FrameData frame;
    presentation.append_cues(frame, clock_time(91ms));
    CHECK(circle_count(frame, dots::presentation::CircleKind::FoodPop) == 0);
}

#include "mycore/core/core.hpp"
#include "mycore/core/strong_id.hpp"
#include "mycore/math/vector2.hpp"
#include "mycore/time/time.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <type_traits>

TEST_CASE("MyCore Core reports its library name", "[core]") {
    REQUIRE(mycore::core::library_name() == "MyCore::Core");
}

namespace {

struct TestEntityTag;
struct TestClientTag;

using TestEntityId = mycore::core::StrongId<TestEntityTag, std::uint32_t>;
using TestClientId = mycore::core::StrongId<TestClientTag, std::uint32_t>;

} // namespace

TEST_CASE("Strong IDs are invalid by default and compare by value", "[core][strong-id]") {
    STATIC_REQUIRE_FALSE(std::is_convertible_v<std::uint32_t, TestEntityId>);
    STATIC_REQUIRE_FALSE(std::is_same_v<TestEntityId, TestClientId>);

    const TestEntityId invalid;
    const TestEntityId first{7};
    const TestEntityId same{7};
    const TestEntityId later{9};

    REQUIRE_FALSE(invalid.is_valid());
    REQUIRE_FALSE(static_cast<bool>(TestEntityId::invalid()));
    REQUIRE(first.is_valid());
    REQUIRE(first.value() == 7);
    REQUIRE(first == same);
    REQUIRE(first < later);
}

TEST_CASE("Vector2 supports Dots movement operations", "[math][vector]") {
    using mycore::math::Vector2;

    const Vector2 movement = Vector2{2.0F, -1.0F} + Vector2{1.0F, 3.0F};
    REQUIRE(movement == Vector2{3.0F, 2.0F});
    REQUIRE((movement - Vector2{1.0F, 1.0F}) == Vector2{2.0F, 1.0F});
    REQUIRE((movement * 2.0F) == Vector2{6.0F, 4.0F});
    REQUIRE((2.0F * movement) == Vector2{6.0F, 4.0F});
    REQUIRE(mycore::math::dot(movement, Vector2{1.0F, 2.0F}) == 7.0F);
    REQUIRE(mycore::math::length_squared(Vector2{3.0F, 4.0F}) == 25.0F);
    REQUIRE(mycore::math::length(Vector2{3.0F, 4.0F}) == Catch::Approx(5.0F));

    const auto normalized = mycore::math::normalized_or_zero(Vector2{3.0F, 4.0F});
    REQUIRE(normalized.x == Catch::Approx(0.6F));
    REQUIRE(normalized.y == Catch::Approx(0.8F));
    REQUIRE(mycore::math::normalized_or_zero(Vector2{}) == Vector2{});
}

TEST_CASE("Ticks support arithmetic and duration conversion", "[time][tick]") {
    using namespace std::chrono_literals;
    using mycore::time::Tick;
    using mycore::time::TickDelta;

    const Tick start{12};
    const Tick end = start + TickDelta{5};

    REQUIRE(end == Tick{17});
    REQUIRE((end - start) == TickDelta{5});
    REQUIRE(mycore::time::duration_to_ticks(75ms, 20ms) == TickDelta{3});
    REQUIRE(mycore::time::ticks_to_duration(TickDelta{3}, 20ms) == 60ms);
}

TEST_CASE("Periodic deadlines discard producer backlog after an overrun",
          "[time][periodic-deadline]") {
    using namespace std::chrono_literals;

    const auto start = mycore::time::MonotonicTimePoint{1s};
    const auto on_time = mycore::time::advance_periodic_deadline(start, 20ms, start + 5ms);
    CHECK(on_time.time == start + 20ms);
    CHECK_FALSE(on_time.discarded_backlog);

    const auto exact_deadline = mycore::time::advance_periodic_deadline(start, 20ms, start + 20ms);
    CHECK(exact_deadline.time == start + 40ms);
    CHECK(exact_deadline.discarded_backlog);

    const auto late = mycore::time::advance_periodic_deadline(start, 20ms, start + 95ms);
    CHECK(late.time == start + 115ms);
    CHECK(late.discarded_backlog);

    CHECK_THROWS_AS(mycore::time::advance_periodic_deadline(start, 0ns, start),
                    std::invalid_argument);
}

TEST_CASE("Fixed-step accumulation reports and can discard capped backlog", "[time][fixed-step]") {
    using namespace std::chrono_literals;

    mycore::time::FixedStepAccumulator accumulator{10ms};

    const auto partial = accumulator.advance(25ms, 8);
    REQUIRE(partial.steps == 2);
    REQUIRE(partial.pending_steps == 0);
    REQUIRE(partial.accumulated_time == 5ms);
    REQUIRE_FALSE(partial.step_limit_reached);

    const auto capped = accumulator.advance(50ms, 3);
    REQUIRE(capped.steps == 3);
    REQUIRE(capped.pending_steps == 2);
    REQUIRE(capped.accumulated_time == 25ms);
    REQUIRE(capped.step_limit_reached);

    const auto catch_up = accumulator.advance(0ms, 8);
    REQUIRE(catch_up.steps == 2);
    REQUIRE(catch_up.pending_steps == 0);
    REQUIRE(catch_up.accumulated_time == 5ms);
    REQUIRE_FALSE(catch_up.step_limit_reached);

    const auto discarded = accumulator.advance(50ms, 3);
    REQUIRE(discarded.step_limit_reached);
    REQUIRE(accumulator.discard_pending_steps() == 20ms);
    REQUIRE(accumulator.accumulated_time() == 5ms);
}

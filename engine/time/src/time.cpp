#include "mycore/time/time.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace mycore::time {

namespace {

void require_positive_step(Duration tick_duration) {
    if (tick_duration <= Duration::zero()) {
        throw std::invalid_argument{"tick duration must be positive"};
    }
}

} // namespace

TickDelta duration_to_ticks(Duration duration, Duration tick_duration) {
    require_positive_step(tick_duration);
    if (duration < Duration::zero()) {
        throw std::invalid_argument{"duration must not be negative"};
    }
    return TickDelta{static_cast<std::uint64_t>(duration / tick_duration)};
}

Duration ticks_to_duration(TickDelta ticks, Duration tick_duration) {
    require_positive_step(tick_duration);
    const auto maximum_ticks = static_cast<std::uint64_t>(
        std::numeric_limits<Duration::rep>::max() / tick_duration.count());
    if (ticks.value() > maximum_ticks) {
        throw std::overflow_error{"tick duration conversion overflow"};
    }
    return tick_duration * static_cast<Duration::rep>(ticks.value());
}

FixedStepAccumulator::FixedStepAccumulator(Duration step_duration)
    : step_duration_(step_duration) {
    require_positive_step(step_duration_);
}

FixedStepResult FixedStepAccumulator::advance(Duration elapsed, std::size_t maximum_steps) {
    if (elapsed < Duration::zero()) {
        throw std::invalid_argument{"elapsed duration must not be negative"};
    }

    if (elapsed > Duration::max() - accumulated_) {
        throw std::overflow_error{"fixed-step accumulator overflow"};
    }
    accumulated_ += elapsed;

    const auto available_steps = static_cast<std::uint64_t>(accumulated_ / step_duration_);
    const auto size_max = std::numeric_limits<std::size_t>::max();
    const auto bounded_available =
        static_cast<std::size_t>(std::min(available_steps, static_cast<std::uint64_t>(size_max)));
    const auto steps = std::min(bounded_available, maximum_steps);
    accumulated_ -= step_duration_ * static_cast<Duration::rep>(steps);
    return {
        .steps = steps,
        .pending_steps = bounded_available - steps,
        .accumulated_time = accumulated_,
        .step_limit_reached = bounded_available > maximum_steps,
    };
}

Duration FixedStepAccumulator::discard_pending_steps() noexcept {
    const auto pending_steps = accumulated_ / step_duration_;
    const auto discarded = step_duration_ * pending_steps;
    accumulated_ -= discarded;
    return discarded;
}

} // namespace mycore::time

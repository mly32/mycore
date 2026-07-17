#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>

namespace mycore::time {

using Duration = std::chrono::nanoseconds;

// A non-negative distance between two simulation ticks.
class TickDelta {
public:
    constexpr TickDelta() noexcept = default;
    explicit constexpr TickDelta(std::uint64_t value) noexcept
        : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

    auto operator<=>(const TickDelta&) const = default;

private:
    std::uint64_t value_{};
};

// A monotonically increasing simulation tick value.
class Tick {
public:
    constexpr Tick() noexcept = default;
    explicit constexpr Tick(std::uint64_t value) noexcept
        : value_(value) {}

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

    constexpr Tick& operator+=(TickDelta delta) noexcept {
        value_ += delta.value();
        return *this;
    }

    auto operator<=>(const Tick&) const = default;

private:
    std::uint64_t value_{};
};

[[nodiscard]] constexpr Tick operator+(Tick tick, TickDelta delta) noexcept {
    return tick += delta;
}

[[nodiscard]] constexpr TickDelta operator-(Tick lhs, Tick rhs) noexcept {
    return TickDelta{lhs.value() - rhs.value()};
}

[[nodiscard]] TickDelta duration_to_ticks(Duration duration, Duration tick_duration);
[[nodiscard]] Duration ticks_to_duration(TickDelta ticks, Duration tick_duration);

// The steps produced by an accumulator advance and its unconsumed time.
struct FixedStepResult {
    std::size_t steps{};
    Duration remainder{};
};

// Converts elapsed time into fixed steps while retaining unconsumed time.
class FixedStepAccumulator {
public:
    explicit FixedStepAccumulator(Duration step_duration);

    [[nodiscard]] FixedStepResult advance(Duration elapsed, std::size_t maximum_steps);
    [[nodiscard]] Duration step_duration() const noexcept {
        return step_duration_;
    }
    [[nodiscard]] Duration remainder() const noexcept {
        return accumulated_;
    }

private:
    Duration step_duration_;
    Duration accumulated_{};
};

} // namespace mycore::time

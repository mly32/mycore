#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mycore::debug {

struct FrameMetricsSnapshot {
    std::size_t sample_count{};
    std::uint64_t total_frame_count{};
    double latest_milliseconds{};
    double average_milliseconds{};
    double minimum_milliseconds{};
    double maximum_milliseconds{};
    double frames_per_second{};
};

class FrameMetrics {
public:
    explicit FrameMetrics(std::size_t capacity = 120);

    void add_sample(std::chrono::nanoseconds duration);
    void reset() noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] FrameMetricsSnapshot snapshot() const noexcept;

private:
    std::vector<double> samples_milliseconds_;
    std::size_t next_index_{};
    std::size_t sample_count_{};
    std::uint64_t total_frame_count_{};
    double latest_milliseconds_{};
};

struct FixedStepMetricsSample {
    std::chrono::nanoseconds frame_duration{};
    std::chrono::nanoseconds simulation_duration{};
    std::chrono::nanoseconds backlog{};
    std::chrono::nanoseconds discarded_time{};
    std::size_t steps{};
    std::size_t pending_steps{};
    bool step_limit_reached{};
};

struct FixedStepMetricsSnapshot {
    std::size_t sample_count{};
    std::uint64_t total_frame_count{};
    std::uint64_t total_step_count{};
    std::uint64_t catch_up_frame_count{};
    std::uint64_t step_limit_hit_count{};
    std::uint64_t deadline_miss_count{};
    std::size_t latest_steps{};
    std::size_t latest_pending_steps{};
    double target_steps_per_second{};
    double actual_steps_per_second{};
    double latest_simulation_milliseconds{};
    double latest_step_milliseconds{};
    double average_step_milliseconds{};
    double maximum_step_milliseconds{};
    double backlog_milliseconds{};
    double latest_discarded_milliseconds{};
    double total_discarded_milliseconds{};
    bool latest_step_limit_reached{};
    bool latest_deadline_missed{};
};

class FixedStepMetrics {
public:
    explicit FixedStepMetrics(std::chrono::nanoseconds step_duration, std::size_t capacity = 120);

    void add_sample(const FixedStepMetricsSample& sample);
    void reset() noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] FixedStepMetricsSnapshot snapshot() const noexcept;

private:
    struct RollingSample {
        double frame_milliseconds{};
        double simulation_milliseconds{};
        std::size_t steps{};
    };

    std::chrono::nanoseconds step_duration_;
    std::vector<RollingSample> samples_;
    std::size_t next_index_{};
    std::size_t sample_count_{};
    std::uint64_t total_frame_count_{};
    std::uint64_t total_step_count_{};
    std::uint64_t catch_up_frame_count_{};
    std::uint64_t step_limit_hit_count_{};
    std::uint64_t deadline_miss_count_{};
    FixedStepMetricsSample latest_;
    double total_discarded_milliseconds_{};
    bool latest_deadline_missed_{};
};

} // namespace mycore::debug

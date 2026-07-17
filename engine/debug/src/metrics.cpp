#include "mycore/debug/metrics.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace mycore::debug {

FrameMetrics::FrameMetrics(std::size_t capacity)
    : samples_milliseconds_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument{"Frame metrics capacity must be positive"};
    }
}

void FrameMetrics::add_sample(std::chrono::nanoseconds duration) {
    if (duration < std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument{"Frame duration must not be negative"};
    }
    latest_milliseconds_ = std::chrono::duration<double, std::milli>{duration}.count();
    samples_milliseconds_[next_index_] = latest_milliseconds_;
    next_index_ = (next_index_ + 1) % samples_milliseconds_.size();
    sample_count_ = std::min(sample_count_ + 1, samples_milliseconds_.size());
    ++total_frame_count_;
}

void FrameMetrics::reset() noexcept {
    next_index_ = 0;
    sample_count_ = 0;
    total_frame_count_ = 0;
    latest_milliseconds_ = 0.0;
}

std::size_t FrameMetrics::capacity() const noexcept {
    return samples_milliseconds_.size();
}

FrameMetricsSnapshot FrameMetrics::snapshot() const noexcept {
    FrameMetricsSnapshot result{
        .sample_count = sample_count_,
        .total_frame_count = total_frame_count_,
        .latest_milliseconds = latest_milliseconds_,
    };
    if (sample_count_ == 0) {
        return result;
    }

    const auto samples = samples_milliseconds_.begin();
    const auto end = samples + static_cast<std::ptrdiff_t>(sample_count_);
    const auto [minimum, maximum] = std::minmax_element(samples, end);
    const auto total = std::accumulate(samples, end, 0.0);
    result.average_milliseconds = total / static_cast<double>(sample_count_);
    result.minimum_milliseconds = *minimum;
    result.maximum_milliseconds = *maximum;
    if (result.average_milliseconds > 0.0) {
        result.frames_per_second = 1000.0 / result.average_milliseconds;
    }
    return result;
}

FixedStepMetrics::FixedStepMetrics(std::chrono::nanoseconds step_duration, std::size_t capacity)
    : step_duration_(step_duration),
      samples_(capacity) {
    if (step_duration <= std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument{"Fixed-step metrics duration must be positive"};
    }
    if (capacity == 0) {
        throw std::invalid_argument{"Fixed-step metrics capacity must be positive"};
    }
}

void FixedStepMetrics::add_sample(const FixedStepMetricsSample& sample) {
    if (sample.frame_duration < std::chrono::nanoseconds::zero() ||
        sample.simulation_duration < std::chrono::nanoseconds::zero() ||
        sample.backlog < std::chrono::nanoseconds::zero() ||
        sample.discarded_time < std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument{"Fixed-step metric durations must not be negative"};
    }

    const auto frame_milliseconds =
        std::chrono::duration<double, std::milli>{sample.frame_duration}.count();
    const auto simulation_milliseconds =
        std::chrono::duration<double, std::milli>{sample.simulation_duration}.count();
    samples_[next_index_] = {
        .frame_milliseconds = frame_milliseconds,
        .simulation_milliseconds = simulation_milliseconds,
        .steps = sample.steps,
    };
    next_index_ = (next_index_ + 1) % samples_.size();
    sample_count_ = std::min(sample_count_ + 1, samples_.size());

    latest_ = sample;
    ++total_frame_count_;
    total_step_count_ += sample.steps;
    if (sample.steps > 1) {
        ++catch_up_frame_count_;
    }
    if (sample.step_limit_reached) {
        ++step_limit_hit_count_;
    }
    const auto simulation_budget =
        std::chrono::duration<double, std::milli>{step_duration_}.count() *
        static_cast<double>(sample.steps);
    latest_deadline_missed_ = sample.steps > 0 && simulation_milliseconds > simulation_budget;
    if (latest_deadline_missed_) {
        ++deadline_miss_count_;
    }
    total_discarded_milliseconds_ +=
        std::chrono::duration<double, std::milli>{sample.discarded_time}.count();
}

void FixedStepMetrics::reset() noexcept {
    next_index_ = 0;
    sample_count_ = 0;
    total_frame_count_ = 0;
    total_step_count_ = 0;
    catch_up_frame_count_ = 0;
    step_limit_hit_count_ = 0;
    deadline_miss_count_ = 0;
    latest_ = {};
    total_discarded_milliseconds_ = 0.0;
    latest_deadline_missed_ = false;
}

std::size_t FixedStepMetrics::capacity() const noexcept {
    return samples_.size();
}

FixedStepMetricsSnapshot FixedStepMetrics::snapshot() const noexcept {
    FixedStepMetricsSnapshot result{
        .sample_count = sample_count_,
        .total_frame_count = total_frame_count_,
        .total_step_count = total_step_count_,
        .catch_up_frame_count = catch_up_frame_count_,
        .step_limit_hit_count = step_limit_hit_count_,
        .deadline_miss_count = deadline_miss_count_,
        .latest_steps = latest_.steps,
        .latest_pending_steps = latest_.pending_steps,
        .target_steps_per_second = 1.0 / std::chrono::duration<double>{step_duration_}.count(),
        .latest_simulation_milliseconds =
            std::chrono::duration<double, std::milli>{latest_.simulation_duration}.count(),
        .backlog_milliseconds = std::chrono::duration<double, std::milli>{latest_.backlog}.count(),
        .latest_discarded_milliseconds =
            std::chrono::duration<double, std::milli>{latest_.discarded_time}.count(),
        .total_discarded_milliseconds = total_discarded_milliseconds_,
        .latest_step_limit_reached = latest_.step_limit_reached,
        .latest_deadline_missed = latest_deadline_missed_,
    };
    if (latest_.steps > 0) {
        result.latest_step_milliseconds =
            result.latest_simulation_milliseconds / static_cast<double>(latest_.steps);
    }
    if (sample_count_ == 0) {
        return result;
    }

    double rolling_frame_milliseconds{};
    double rolling_simulation_milliseconds{};
    std::size_t rolling_steps{};
    for (std::size_t index = 0; index < sample_count_; ++index) {
        const auto& sample = samples_[index];
        rolling_frame_milliseconds += sample.frame_milliseconds;
        rolling_steps += sample.steps;
        if (sample.steps > 0) {
            rolling_simulation_milliseconds += sample.simulation_milliseconds;
            result.maximum_step_milliseconds =
                std::max(result.maximum_step_milliseconds,
                         sample.simulation_milliseconds / static_cast<double>(sample.steps));
        }
    }
    if (rolling_frame_milliseconds > 0.0) {
        result.actual_steps_per_second =
            static_cast<double>(rolling_steps) * 1000.0 / rolling_frame_milliseconds;
    }
    if (rolling_steps > 0) {
        result.average_step_milliseconds =
            rolling_simulation_milliseconds / static_cast<double>(rolling_steps);
    }
    return result;
}

} // namespace mycore::debug

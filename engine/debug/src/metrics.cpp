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

} // namespace mycore::debug

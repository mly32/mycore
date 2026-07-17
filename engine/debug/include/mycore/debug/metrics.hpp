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

} // namespace mycore::debug

#pragma once

#include "mycore/time/time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace dots::client_runtime {

struct CommandTimingSettings {
    double target_depth{2.0};
    double deadband_minimum{1.5};
    double deadband_maximum{2.5};
    double ewma_alpha{0.125};
    double gain{0.025};
    double minimum_rate_scale{0.95};
    double maximum_rate_scale{1.05};
    std::size_t initial_prefill_count{2};
};

struct CommandTimingStatistics {
    double target_depth{};
    std::optional<std::uint8_t> latest_depth;
    double smoothed_depth{};
    double rate_scale{1.0};
    std::uint64_t observation_count{};
    std::uint64_t low_depth_observation_count{};
    std::uint64_t high_depth_observation_count{};
    mycore::time::Duration accumulated_phase_correction{};
    std::size_t prefill_input_count{};
    std::uint64_t discarded_backlog_count{};
};

// Dots command timing is intentionally separate from gameplay time. It only controls when the
// next fixed-tick input sample is produced from accepted server queue-depth observations.
class CommandTimingController {
public:
    explicit CommandTimingController(CommandTimingSettings settings = {});

    void observe_server_queue_depth(std::uint8_t depth) noexcept;
    [[nodiscard]] mycore::time::Duration scale_accumulator_elapsed(mycore::time::Duration elapsed);
    [[nodiscard]] mycore::time::Duration next_period(mycore::time::Duration nominal_period);
    void record_prefill_inputs(std::size_t count) noexcept;
    void record_discarded_backlog() noexcept;

    [[nodiscard]] std::size_t initial_prefill_count() const noexcept;
    [[nodiscard]] const CommandTimingStatistics& statistics() const noexcept;

private:
    [[nodiscard]] mycore::time::Duration scaled_duration(mycore::time::Duration value,
                                                         double scale) const;
    void add_phase_correction(mycore::time::Duration correction) noexcept;
    void update_rate_scale() noexcept;

    CommandTimingSettings settings_;
    CommandTimingStatistics statistics_;
};

} // namespace dots::client_runtime

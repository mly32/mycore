#include "dots/client_runtime/command_timing.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace dots::client_runtime {
namespace {

[[nodiscard]] bool valid_settings(const CommandTimingSettings& settings) noexcept {
    return std::isfinite(settings.target_depth) && settings.target_depth >= 0.0 &&
           std::isfinite(settings.deadband_minimum) &&
           settings.deadband_minimum <= settings.target_depth &&
           std::isfinite(settings.deadband_maximum) &&
           settings.deadband_maximum >= settings.target_depth &&
           settings.deadband_minimum <= settings.deadband_maximum &&
           std::isfinite(settings.ewma_alpha) && settings.ewma_alpha > 0.0 &&
           settings.ewma_alpha <= 1.0 && std::isfinite(settings.gain) && settings.gain >= 0.0 &&
           std::isfinite(settings.minimum_rate_scale) && settings.minimum_rate_scale > 0.0 &&
           std::isfinite(settings.maximum_rate_scale) &&
           settings.maximum_rate_scale >= settings.minimum_rate_scale &&
           settings.minimum_rate_scale <= 1.0 && settings.maximum_rate_scale >= 1.0 &&
           settings.initial_prefill_count > 0;
}

} // namespace

CommandTimingController::CommandTimingController(CommandTimingSettings settings)
    : settings_(settings),
      statistics_{} {
    if (!valid_settings(settings_)) {
        throw std::invalid_argument{"invalid command timing settings"};
    }
    statistics_.target_depth = settings_.target_depth;
    statistics_.smoothed_depth = settings_.target_depth;
}

void CommandTimingController::observe_server_queue_depth(std::uint8_t depth) noexcept {
    statistics_.latest_depth = depth;
    ++statistics_.observation_count;
    if (static_cast<double>(depth) < settings_.deadband_minimum) {
        ++statistics_.low_depth_observation_count;
    } else if (static_cast<double>(depth) > settings_.deadband_maximum) {
        ++statistics_.high_depth_observation_count;
    }
    statistics_.smoothed_depth +=
        settings_.ewma_alpha * (static_cast<double>(depth) - statistics_.smoothed_depth);
    update_rate_scale();
}

mycore::time::Duration
CommandTimingController::scale_accumulator_elapsed(mycore::time::Duration elapsed) {
    if (elapsed < mycore::time::Duration::zero()) {
        throw std::invalid_argument{"command accumulator elapsed time must not be negative"};
    }
    const auto scaled = scaled_duration(elapsed, statistics_.rate_scale);
    add_phase_correction(scaled - elapsed);
    return scaled;
}

mycore::time::Duration CommandTimingController::next_period(mycore::time::Duration nominal_period) {
    if (nominal_period <= mycore::time::Duration::zero()) {
        throw std::invalid_argument{"nominal command period must be positive"};
    }
    const auto scaled = scaled_duration(nominal_period, 1.0 / statistics_.rate_scale);
    if (scaled <= mycore::time::Duration::zero()) {
        throw std::invalid_argument{"scaled command period is shorter than clock resolution"};
    }
    add_phase_correction(nominal_period - scaled);
    return scaled;
}

void CommandTimingController::record_prefill_inputs(std::size_t count) noexcept {
    statistics_.prefill_input_count += count;
}

void CommandTimingController::record_discarded_backlog() noexcept {
    ++statistics_.discarded_backlog_count;
}

std::size_t CommandTimingController::initial_prefill_count() const noexcept {
    return settings_.initial_prefill_count;
}

const CommandTimingStatistics& CommandTimingController::statistics() const noexcept {
    return statistics_;
}

mycore::time::Duration CommandTimingController::scaled_duration(mycore::time::Duration value,
                                                                double scale) const {
    const auto scaled = static_cast<long double>(value.count()) * static_cast<long double>(scale);
    const auto minimum =
        static_cast<long double>(std::numeric_limits<mycore::time::Duration::rep>::min());
    const auto maximum =
        static_cast<long double>(std::numeric_limits<mycore::time::Duration::rep>::max());
    if (!std::isfinite(scaled) || scaled < minimum || scaled > maximum) {
        throw std::overflow_error{"scaled command duration overflow"};
    }
    return mycore::time::Duration{static_cast<mycore::time::Duration::rep>(std::llround(scaled))};
}

void CommandTimingController::add_phase_correction(mycore::time::Duration correction) noexcept {
    const auto result = static_cast<long double>(statistics_.accumulated_phase_correction.count()) +
                        static_cast<long double>(correction.count());
    const auto maximum =
        static_cast<long double>(std::numeric_limits<mycore::time::Duration::rep>::max());
    const auto minimum =
        static_cast<long double>(std::numeric_limits<mycore::time::Duration::rep>::min());
    if (result > maximum) {
        statistics_.accumulated_phase_correction = mycore::time::Duration::max();
    } else if (result < minimum) {
        statistics_.accumulated_phase_correction = mycore::time::Duration::min();
    } else {
        statistics_.accumulated_phase_correction =
            mycore::time::Duration{static_cast<mycore::time::Duration::rep>(result)};
    }
}

void CommandTimingController::update_rate_scale() noexcept {
    if (statistics_.smoothed_depth >= settings_.deadband_minimum &&
        statistics_.smoothed_depth <= settings_.deadband_maximum) {
        statistics_.rate_scale = 1.0;
        return;
    }
    statistics_.rate_scale =
        std::clamp(1.0 + settings_.gain * (settings_.target_depth - statistics_.smoothed_depth),
                   settings_.minimum_rate_scale,
                   settings_.maximum_rate_scale);
}

} // namespace dots::client_runtime

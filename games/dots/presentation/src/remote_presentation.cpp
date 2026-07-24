#include "dots/presentation/remote_presentation.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace dots::presentation {
namespace {

constexpr double kServerTicksPerSecond = 30.0;
constexpr double kCursorDeadbandTicks = 0.25;
constexpr double kMaximumCursorErrorTicks = 6.0;
constexpr double kMinimumCursorRate = 0.95;
constexpr double kMaximumCursorRate = 1.05;

[[nodiscard]] bool valid_entity(const protocol::EntityState& entity) noexcept {
    return entity.entity_id.is_valid() && std::isfinite(entity.position_x) &&
           std::isfinite(entity.position_y) && std::isfinite(entity.mass) && entity.mass > 0.0F;
}

[[nodiscard]] mycore::math::Vector2 position(const protocol::EntityState& entity) noexcept {
    return {entity.position_x, entity.position_y};
}

[[nodiscard]] float interpolate(float older, float newer, float alpha) noexcept {
    return older + ((newer - older) * alpha);
}

} // namespace

void RemoteSnapshotBuffer::insert(RemoteSnapshotSample sample) {
    if (!sample.snapshot_id.is_valid() || std::any_of(sample.entities.begin(),
                                                      sample.entities.end(),
                                                      [](const protocol::EntityState& entity) {
                                                          return !valid_entity(entity);
                                                      })) {
        return;
    }
    std::sort(sample.entities.begin(),
              sample.entities.end(),
              [](const protocol::EntityState& lhs, const protocol::EntityState& rhs) {
                  return lhs.entity_id < rhs.entity_id;
              });
    if (std::adjacent_find(sample.entities.begin(),
                           sample.entities.end(),
                           [](const protocol::EntityState& lhs, const protocol::EntityState& rhs) {
                               return lhs.entity_id == rhs.entity_id;
                           }) != sample.entities.end()) {
        return;
    }
    const auto first_sample = samples_.empty();
    if (!first_sample) {
        const auto& newest = samples_.back();
        if (sample.snapshot_id <= newest.snapshot_id || sample.server_tick <= newest.server_tick) {
            return;
        }
        const auto expected = std::chrono::duration<double>{
            static_cast<double>(sample.server_tick - newest.server_tick) / kServerTicksPerSecond};
        const auto observed = sample.arrival_time - newest.arrival_time;
        const auto jitter = std::abs(std::chrono::duration<double, std::milli>{observed}.count() -
                                     std::chrono::duration<double, std::milli>{expected}.count());
        latest_jitter_milliseconds_ = jitter;
        ewma_jitter_milliseconds_ += (jitter - ewma_jitter_milliseconds_) / 16.0;
    }
    if (ready_ && static_cast<double>(sample.server_tick) <= presentation_tick_) {
        ++late_snapshot_count_;
    }
    samples_.push_back(std::move(sample));
    if (first_sample) {
        presentation_tick_ = static_cast<double>(samples_.back().server_tick);
    }
    if (samples_.size() > kRemoteSnapshotCapacity) {
        samples_.erase(samples_.begin());
    }
}

void RemoteSnapshotBuffer::advance(std::chrono::steady_clock::time_point now) {
    if (!last_advance_time_) {
        last_advance_time_ = now;
    }
    if (samples_.empty()) {
        return;
    }
    const auto newest_tick = samples_.back().server_tick;
    const auto coverage = newest_tick - samples_.front().server_tick;
    if (!ready_) {
        if (coverage < kRemotePresentationDelayTicks) {
            last_advance_time_ = now;
            return;
        }
        presentation_tick_ = static_cast<double>(newest_tick - kRemotePresentationDelayTicks);
        ready_ = true;
        last_advance_time_ = now;
        return;
    }

    const auto desired_tick = static_cast<double>(newest_tick - kRemotePresentationDelayTicks);
    if (hold_started_at_ && !newer_sample_index()) {
        cursor_rate_ = 0.0;
        cursor_error_ = desired_tick - presentation_tick_;
        rate_correction_active_ = false;
        last_advance_time_ = now;
        return;
    }
    if (hold_started_at_) {
        finish_hold(now);
    }

    auto rebased = false;
    if (presentation_tick_ < static_cast<double>(samples_.front().server_tick)) {
        presentation_tick_ =
            std::max(desired_tick, static_cast<double>(samples_.front().server_tick));
        cursor_rate_ = 1.0;
        rate_correction_active_ = false;
        ++hard_rebase_count_;
        last_advance_time_ = now;
        rebased = true;
    }
    cursor_error_ = desired_tick - presentation_tick_;
    if (!rebased && cursor_error_ > kMaximumCursorErrorTicks) {
        presentation_tick_ = desired_tick;
        cursor_rate_ = 1.0;
        rate_correction_active_ = false;
        ++hard_rebase_count_;
        last_advance_time_ = now;
        rebased = true;
    } else if (!rebased && std::abs(cursor_error_) <= kCursorDeadbandTicks) {
        cursor_rate_ = 1.0;
        rate_correction_active_ = false;
    } else if (!rebased) {
        cursor_rate_ =
            std::clamp(1.0 + (0.02 * cursor_error_), kMinimumCursorRate, kMaximumCursorRate);
        if (!rate_correction_active_) {
            ++rate_correction_count_;
            rate_correction_active_ = true;
        }
    }

    if (!rebased) {
        const auto elapsed = std::max(now - last_advance_time_.value_or(now),
                                      std::chrono::steady_clock::duration::zero());
        presentation_tick_ +=
            std::chrono::duration<double>{elapsed}.count() * kServerTicksPerSecond * cursor_rate_;
    }
    last_advance_time_ = now;
    cursor_error_ = desired_tick - presentation_tick_;
    if (!newer_sample_index()) {
        if (!hold_started_at_) {
            hold_started_at_ = now;
            ++hold_episode_count_;
        }
        cursor_rate_ = 0.0;
        rate_correction_active_ = false;
    }
}

RemotePresentationFrame RemoteSnapshotBuffer::sample(protocol::EntityId controlled_entity_id) {
    RemotePresentationFrame frame{.entities = {},
                                  .bracket = std::nullopt,
                                  .presentation_tick = presentation_tick_,
                                  .ready = ready_,
                                  .holding = hold_started_at_.has_value()};
    if (samples_.empty()) {
        return frame;
    }
    const auto older_index = older_sample_index();
    if (!older_index) {
        return frame;
    }
    const auto newer_index = newer_sample_index();
    const auto& older = samples_[*older_index];
    const auto* newer = newer_index ? &samples_[*newer_index] : nullptr;
    auto alpha = 0.0F;
    if (newer != nullptr) {
        const auto tick_span = newer->server_tick - older.server_tick;
        alpha = tick_span == 0
                    ? 0.0F
                    : std::clamp(static_cast<float>((presentation_tick_ - older.server_tick) /
                                                    static_cast<double>(tick_span)),
                                 0.0F,
                                 1.0F);
        frame.bracket = RemoteBracket{
            .older_snapshot_id = older.snapshot_id,
            .older_server_tick = older.server_tick,
            .newer_snapshot_id = newer->snapshot_id,
            .newer_server_tick = newer->server_tick,
            .alpha = alpha,
        };
    }

    const auto append = [&frame, controlled_entity_id](const protocol::EntityState& entity,
                                                       mycore::math::Vector2 entity_position,
                                                       float mass) {
        if (entity.entity_id != controlled_entity_id) {
            frame.entities.push_back({
                .entity_id = entity.entity_id,
                .kind = entity.kind,
                .position = entity_position,
                .mass = mass,
            });
        }
    };
    if (newer == nullptr) {
        for (const auto& entity : older.entities) {
            append(entity, position(entity), entity.mass);
        }
        record_presented_lifecycle(frame.entities);
        return frame;
    }

    auto older_it = older.entities.begin();
    auto newer_it = newer->entities.begin();
    while (older_it != older.entities.end() || newer_it != newer->entities.end()) {
        if (newer_it == newer->entities.end() ||
            (older_it != older.entities.end() && older_it->entity_id < newer_it->entity_id)) {
            if (presentation_tick_ < newer->server_tick) {
                append(*older_it, position(*older_it), older_it->mass);
            }
            ++older_it;
        } else if (older_it == older.entities.end() || newer_it->entity_id < older_it->entity_id) {
            if (presentation_tick_ >= newer->server_tick) {
                append(*newer_it, position(*newer_it), newer_it->mass);
            }
            ++newer_it;
        } else {
            if (older_it->kind == newer_it->kind) {
                const auto older_position = position(*older_it);
                const auto newer_position = position(*newer_it);
                append(*older_it,
                       older_position + ((newer_position - older_position) * alpha),
                       interpolate(older_it->mass, newer_it->mass, alpha));
            }
            ++older_it;
            ++newer_it;
        }
    }
    record_presented_lifecycle(frame.entities);
    return frame;
}

RemoteEntityEndpoints RemoteSnapshotBuffer::endpoints(protocol::EntityId entity_id) const {
    RemoteEntityEndpoints result;
    const auto older_index = older_sample_index();
    if (!older_index) {
        return result;
    }
    const auto find =
        [entity_id](const RemoteSnapshotSample& sample) -> const protocol::EntityState* {
        const auto iterator =
            std::lower_bound(sample.entities.begin(),
                             sample.entities.end(),
                             entity_id,
                             [](const protocol::EntityState& entity, protocol::EntityId value) {
                                 return entity.entity_id < value;
                             });
        return iterator != sample.entities.end() && iterator->entity_id == entity_id ? &*iterator
                                                                                     : nullptr;
    };
    const auto to_remote = [](const protocol::EntityState& entity) {
        return RemoteEntitySample{
            .entity_id = entity.entity_id,
            .kind = entity.kind,
            .position = position(entity),
            .mass = entity.mass,
        };
    };
    if (const auto* older = find(samples_[*older_index])) {
        result.older = to_remote(*older);
    }
    if (const auto newer_index = newer_sample_index()) {
        if (const auto* newer = find(samples_[*newer_index])) {
            result.newer = to_remote(*newer);
        }
    }
    return result;
}

RemotePresentationStatistics
RemoteSnapshotBuffer::statistics(std::chrono::steady_clock::time_point now) const {
    auto current_hold_duration = std::chrono::steady_clock::duration::zero();
    if (hold_started_at_) {
        current_hold_duration =
            std::max(now - *hold_started_at_, std::chrono::steady_clock::duration::zero());
    }
    const auto maximum_hold_duration = std::max(maximum_hold_duration_, current_hold_duration);
    const auto total_hold_duration = total_hold_duration_ + current_hold_duration;
    RemotePresentationStatistics result{
        .sample_count = samples_.size(),
        .coverage_ticks =
            samples_.empty() ? 0U : samples_.back().server_tick - samples_.front().server_tick,
        .coverage_milliseconds =
            samples_.empty()
                ? 0.0
                : (1000.0 *
                   static_cast<double>(samples_.back().server_tick - samples_.front().server_tick) /
                   kServerTicksPerSecond),
        .current_delay_ticks = samples_.empty() ? 0.0
                                                : static_cast<double>(samples_.back().server_tick) -
                                                      presentation_tick_,
        .presentation_tick = presentation_tick_,
        .cursor_rate = cursor_rate_,
        .cursor_error = cursor_error_,
        .bracket = std::nullopt,
        .latest_jitter_milliseconds = latest_jitter_milliseconds_,
        .ewma_jitter_milliseconds = ewma_jitter_milliseconds_,
        .late_snapshot_count = late_snapshot_count_,
        .hold_episode_count = hold_episode_count_,
        .holding = hold_started_at_.has_value(),
        .current_hold_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(current_hold_duration),
        .last_hold_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(last_hold_duration_),
        .maximum_hold_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(maximum_hold_duration),
        .total_hold_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(total_hold_duration),
        .hold_recovery_count = hold_recovery_count_,
        .rate_correction_count = rate_correction_count_,
        .hard_rebase_count = hard_rebase_count_,
        .delayed_entity_create_count = delayed_entity_create_count_,
        .delayed_entity_remove_count = delayed_entity_remove_count_,
    };
    const auto older_index = older_sample_index();
    const auto newer_index = newer_sample_index();
    if (older_index && newer_index) {
        const auto& older = samples_[*older_index];
        const auto& newer = samples_[*newer_index];
        const auto span = newer.server_tick - older.server_tick;
        result.bracket = RemoteBracket{
            .older_snapshot_id = older.snapshot_id,
            .older_server_tick = older.server_tick,
            .newer_snapshot_id = newer.snapshot_id,
            .newer_server_tick = newer.server_tick,
            .alpha = span == 0
                         ? 0.0F
                         : std::clamp(static_cast<float>((presentation_tick_ - older.server_tick) /
                                                         static_cast<double>(span)),
                                      0.0F,
                                      1.0F),
        };
    }
    return result;
}

void RemoteSnapshotBuffer::finish_hold(std::chrono::steady_clock::time_point now) noexcept {
    if (!hold_started_at_) {
        return;
    }
    last_hold_duration_ =
        std::max(now - *hold_started_at_, std::chrono::steady_clock::duration::zero());
    maximum_hold_duration_ = std::max(maximum_hold_duration_, last_hold_duration_);
    total_hold_duration_ += last_hold_duration_;
    ++hold_recovery_count_;
    hold_started_at_.reset();
}

void RemoteSnapshotBuffer::record_presented_lifecycle(
    const std::vector<RemoteEntitySample>& entities) {
    std::vector<PresentedEntityIdentity> current_entities;
    current_entities.reserve(entities.size());
    for (const auto& entity : entities) {
        current_entities.push_back({
            .entity_id = entity.entity_id,
            .kind = entity.kind,
        });
    }
    if (!lifecycle_initialized_) {
        last_presented_entities_ = std::move(current_entities);
        lifecycle_initialized_ = true;
        return;
    }

    auto previous = last_presented_entities_.begin();
    auto current = current_entities.begin();
    while (previous != last_presented_entities_.end() || current != current_entities.end()) {
        if (current == current_entities.end() || (previous != last_presented_entities_.end() &&
                                                  previous->entity_id < current->entity_id)) {
            ++delayed_entity_remove_count_;
            ++previous;
        } else if (previous == last_presented_entities_.end() ||
                   current->entity_id < previous->entity_id) {
            ++delayed_entity_create_count_;
            ++current;
        } else {
            if (previous->kind != current->kind) {
                ++delayed_entity_remove_count_;
                ++delayed_entity_create_count_;
            }
            ++previous;
            ++current;
        }
    }
    last_presented_entities_ = std::move(current_entities);
}

std::optional<std::size_t> RemoteSnapshotBuffer::older_sample_index() const noexcept {
    if (samples_.empty()) {
        return std::nullopt;
    }
    auto result = std::size_t{0};
    for (std::size_t index = 1; index < samples_.size(); ++index) {
        if (static_cast<double>(samples_[index].server_tick) > presentation_tick_) {
            break;
        }
        result = index;
    }
    return result;
}

std::optional<std::size_t> RemoteSnapshotBuffer::newer_sample_index() const noexcept {
    for (std::size_t index = 0; index < samples_.size(); ++index) {
        if (static_cast<double>(samples_[index].server_tick) > presentation_tick_) {
            return index;
        }
    }
    return std::nullopt;
}

} // namespace dots::presentation

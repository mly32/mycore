#include "dots/prediction/prediction.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array kEntityCounts{
    std::size_t{10}, std::size_t{100}, std::size_t{500}, std::size_t{1'000}};
constexpr std::array kReplayCases{
    std::pair{std::uint32_t{100}, std::size_t{3}},
    std::pair{std::uint32_t{200}, std::size_t{6}},
    std::pair{std::uint32_t{400}, std::size_t{12}},
};

struct Options {
    std::size_t iterations{50};
};

struct Measurement {
    std::size_t entity_count{};
    std::uint32_t equivalent_rtt_ms{};
    std::size_t replay_count{};
    std::size_t checkpoint_bytes{};
    std::size_t topology_event_count{};
    double p50_ms{};
    double p95_ms{};
    double p99_ms{};
    double maximum_ms{};
    double over_frame_budget_percent{};
};

[[nodiscard]] Options parse_options(std::span<char*> arguments) {
    Options result;
    for (auto index = std::size_t{1}; index < arguments.size(); ++index) {
        const auto argument = std::string_view{arguments[index]};
        if (argument == "--help") {
            std::cout << "Usage: dots_rollback_workload [--iterations <positive integer>]\n";
            std::exit(0);
        }
        if (argument != "--iterations" || index + 1U >= arguments.size()) {
            throw std::invalid_argument{"unknown or incomplete argument"};
        }
        const auto value = std::string_view{arguments[++index]};
        const auto* begin = value.data();
        const auto* end = begin + value.size();
        const auto parsed = std::from_chars(begin, end, result.iterations);
        if (parsed.ec != std::errc{} || parsed.ptr != end || result.iterations == 0) {
            throw std::invalid_argument{"--iterations must be a positive integer"};
        }
    }
    return result;
}

[[nodiscard]] dots::simulation::WorldCheckpoint make_checkpoint(std::size_t entity_count) {
    if (entity_count == 0 || entity_count >= dots::simulation::EntityId::kInvalidValue) {
        throw std::invalid_argument{"invalid workload entity count"};
    }
    const auto owner_id = dots::simulation::PlayerOwnerId{0};
    dots::simulation::WorldCheckpoint result{
        .rules = {},
        .tick = mycore::time::Tick{0},
        .next_entity_id = static_cast<std::uint32_t>(entity_count),
        .owners = {{
            .owner_id = owner_id,
            .player_ids = {dots::simulation::EntityId{0}},
            .movement = {},
            .last_non_zero_movement = {},
            .last_input_id = {},
            .split_cooldown_end_tick = mycore::time::Tick{0},
        }},
        .players = {{
            .entity_id = dots::simulation::EntityId{0},
            .owner_id = owner_id,
            .position = {},
            .mass = 32.0F,
            .launch_velocity = {},
            .merge_eligible_tick = mycore::time::Tick{0},
            .prediction_key = std::nullopt,
        }},
        .food = {},
    };
    result.food.reserve(entity_count - 1U);
    for (auto index = std::size_t{1}; index < entity_count; ++index) {
        const auto column = static_cast<float>(index % 100U);
        const auto row_index = index / 100U;
        const auto row = static_cast<float>(row_index);
        result.food.push_back({
            .entity_id = dots::simulation::EntityId{static_cast<std::uint32_t>(index)},
            .position = {100.0F + (column * 3.0F), 100.0F + (row * 3.0F)},
        });
    }
    dots::simulation::World validation;
    if (validation.restore(result)) {
        throw std::runtime_error{"workload checkpoint validation failed"};
    }
    return result;
}

[[nodiscard]] dots::prediction::PredictionScope
make_scope(const dots::simulation::WorldCheckpoint& checkpoint, std::size_t replay_count) {
    auto built = dots::prediction::build_prediction_scope(
        checkpoint,
        {
            .profile = dots::prediction::PredictionProfile::FullReplicated,
            .mechanics = dots::prediction::kCurrentPredictionMechanics,
            .owned_owner_ids = {dots::simulation::PlayerOwnerId{0}},
            .subscribed_event_owner_ids = {dots::simulation::PlayerOwnerId{0}},
            .replay_horizon = mycore::time::TickDelta{static_cast<std::uint64_t>(replay_count)},
            .scope_epoch = mycore::rollback::ScopeEpoch{1},
            .coverage = {},
        });
    const auto* scope = std::get_if<dots::prediction::PredictionScope>(&built);
    if (scope == nullptr) {
        throw std::runtime_error{"workload scope build failed"};
    }
    return *scope;
}

[[nodiscard]] dots::prediction::TickStimulus stimulus(std::uint32_t input_id,
                                                      bool split_requested) {
    return {
        .commands = {{
            .type = dots::simulation::TickCommandType::ApplyInput,
            .input_id = dots::simulation::InputCommandId{input_id},
            .owner_id = dots::simulation::PlayerOwnerId{0},
            .movement = {1.0F, 0.0F},
            .split_requested = split_requested,
        }},
        .remote_movement_assumptions = {},
    };
}

[[nodiscard]] dots::prediction::Commit require_commit(dots::prediction::CommitResult result,
                                                      std::string_view operation) {
    auto* commit = std::get_if<dots::prediction::Commit>(&result);
    if (commit == nullptr) {
        throw std::runtime_error{std::string{operation}};
    }
    return std::move(*commit);
}

[[nodiscard]] std::size_t
checkpoint_storage_bytes(const dots::simulation::WorldCheckpoint& checkpoint) noexcept {
    auto result = sizeof(checkpoint);
    result += checkpoint.owners.size() * sizeof(dots::simulation::OwnerCheckpoint);
    result += checkpoint.players.size() * sizeof(dots::simulation::PlayerCheckpoint);
    result += checkpoint.food.size() * sizeof(dots::simulation::FoodCheckpoint);
    for (const auto& owner : checkpoint.owners) {
        result += owner.player_ids.size() * sizeof(dots::simulation::EntityId);
    }
    return result;
}

[[nodiscard]] double percentile(const std::vector<double>& sorted, double fraction) {
    const auto rank =
        static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(sorted.size())));
    return sorted[std::clamp(rank, std::size_t{1}, sorted.size()) - 1U];
}

[[nodiscard]] Measurement measure(std::size_t entity_count,
                                  std::uint32_t equivalent_rtt_ms,
                                  std::size_t replay_count,
                                  std::size_t iterations) {
    const auto checkpoint = make_checkpoint(entity_count);
    const auto scope = make_scope(checkpoint, replay_count);
    dots::prediction::Timeline prepared{
        dots::prediction::WorldModel{},
        {.capacity = replay_count + 2U},
    };
    static_cast<void>(require_commit(prepared.initialize(
                                         {
                                             .tick = checkpoint.tick,
                                             .acknowledged_through = std::nullopt,
                                             .scope_epoch = scope.scope_epoch,
                                             .checkpoint = checkpoint,
                                             .events = {},
                                         },
                                         scope),
                                     "workload timeline initialization failed"));

    for (auto sequence = std::size_t{1}; sequence <= replay_count + 1U; ++sequence) {
        const auto input_id = static_cast<std::uint32_t>(sequence);
        static_cast<void>(
            require_commit(prepared.advance(mycore::rollback::CommandSequence{sequence},
                                            stimulus(input_id, sequence == 2U)),
                           "workload prediction advance failed"));
    }
    const auto expected_digest =
        dots::prediction::checkpoint_digest(prepared.state()->checkpoint());

    dots::simulation::World authority_world;
    if (authority_world.restore(checkpoint)) {
        throw std::runtime_error{"workload authority restore failed"};
    }
    const auto first_stimulus = stimulus(1, false);
    const auto first_tick = authority_world.advance(first_stimulus.commands);
    if (!std::holds_alternative<dots::simulation::TickJournal>(first_tick)) {
        throw std::runtime_error{"workload authority advance failed"};
    }
    const auto authority_checkpoint = authority_world.checkpoint();
    const dots::prediction::AuthorityFrame authority{
        .tick = authority_checkpoint.tick,
        .acknowledged_through = mycore::rollback::CommandSequence{1},
        .scope_epoch = scope.scope_epoch,
        .checkpoint = authority_checkpoint,
        .events = {},
    };

    std::vector<double> samples;
    samples.reserve(iterations);
    auto topology_event_count = std::size_t{};
    auto over_frame_budget_count = std::size_t{};
    for (auto iteration = std::size_t{}; iteration < iterations + 3U; ++iteration) {
        auto timeline = prepared;
        const auto started_at = Clock::now();
        auto reconciled = timeline.reconcile(authority);
        const auto elapsed = Clock::now() - started_at;
        const auto commit = require_commit(std::move(reconciled), "workload reconcile failed");
        if (commit.replayed_frame_count != replay_count ||
            commit.predicted_digest != expected_digest) {
            throw std::runtime_error{"workload replay was not deterministic"};
        }
        if (iteration < 3U) {
            continue;
        }
        topology_event_count = 0;
        for (const auto& frame : timeline.history()) {
            topology_event_count +=
                static_cast<std::size_t>(std::ranges::count_if(frame.events, [](const auto& event) {
                    return std::holds_alternative<dots::simulation::PlayerSplit>(event);
                }));
        }
        const auto elapsed_ms = std::chrono::duration<double, std::milli>{elapsed}.count();
        if (elapsed_ms > (1'000.0 / 30.0)) {
            ++over_frame_budget_count;
        }
        samples.push_back(elapsed_ms);
    }
    std::sort(samples.begin(), samples.end());
    return {
        .entity_count = entity_count,
        .equivalent_rtt_ms = equivalent_rtt_ms,
        .replay_count = replay_count,
        .checkpoint_bytes = checkpoint_storage_bytes(checkpoint),
        .topology_event_count = topology_event_count,
        .p50_ms = percentile(samples, 0.50),
        .p95_ms = percentile(samples, 0.95),
        .p99_ms = percentile(samples, 0.99),
        .maximum_ms = samples.back(),
        .over_frame_budget_percent = 100.0 * static_cast<double>(over_frame_budget_count) /
                                     static_cast<double>(samples.size()),
    };
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options({argv, static_cast<std::size_t>(argc)});
        std::cout << "entities,rtt_ms,replay_ticks,checkpoint_bytes,topology_events,"
                     "p50_ms,p95_ms,p99_ms,max_ms,over_33ms_percent\n";
        for (const auto entity_count : kEntityCounts) {
            for (const auto [rtt_ms, replay_count] : kReplayCases) {
                const auto result = measure(entity_count, rtt_ms, replay_count, options.iterations);
                std::cout << result.entity_count << ',' << result.equivalent_rtt_ms << ','
                          << result.replay_count << ',' << result.checkpoint_bytes << ','
                          << result.topology_event_count << ',' << result.p50_ms << ','
                          << result.p95_ms << ',' << result.p99_ms << ',' << result.maximum_ms
                          << ',' << result.over_frame_budget_percent << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dots_rollback_workload: " << error.what() << '\n';
        return 1;
    }
}

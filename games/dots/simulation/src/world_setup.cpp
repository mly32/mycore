#include "dots/simulation/world_setup.hpp"

#include "dots/simulation/world.hpp"

#include <cstdint>
#include <limits>

namespace dots::simulation {
namespace {

std::uint64_t integer_square_root(std::uint64_t value) noexcept {
    std::uint64_t result{};
    std::uint64_t bit = std::uint64_t{1} << 62U;
    while (bit > value) {
        bit >>= 2U;
    }

    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1U) + bit;
        } else {
            result >>= 1U;
        }
        bit >>= 2U;
    }
    return result;
}

} // namespace

bool spawn_default_food_field(World& world) {
    constexpr float kSpacing = 8.0F;
    for (int row = -6; row <= 6; ++row) {
        for (int column = -10; column <= 10; ++column) {
            if (row == 0 && column == 0) {
                continue;
            }
            if (!world.spawn_food(
                    {static_cast<float>(column) * kSpacing, static_cast<float>(row) * kSpacing})) {
                return false;
            }
        }
    }
    return true;
}

mycore::math::Vector2 initial_player_spawn_candidate(std::uint64_t ordinal) noexcept {
    if (ordinal == 0) {
        return {};
    }

    const auto ring = (integer_square_root(ordinal) + 1U) / 2U;
    const auto ring_diameter = (ring * 2U) - 1U;
    const auto ring_start = ring_diameter * ring_diameter;
    const auto offset = ordinal - ring_start;
    const auto edge_length = ring * 2U;

    std::int64_t lattice_x{};
    std::int64_t lattice_y{};
    const auto signed_ring = static_cast<std::int64_t>(ring);
    if (offset < edge_length) {
        lattice_x = signed_ring;
        lattice_y = -signed_ring + 1 + static_cast<std::int64_t>(offset);
    } else if (offset < edge_length * 2U) {
        lattice_x = signed_ring - 1 - static_cast<std::int64_t>(offset - edge_length);
        lattice_y = signed_ring;
    } else if (offset < edge_length * 3U) {
        lattice_x = -signed_ring;
        lattice_y = signed_ring - 1 - static_cast<std::int64_t>(offset - (edge_length * 2U));
    } else {
        lattice_x = -signed_ring + 1 + static_cast<std::int64_t>(offset - (edge_length * 3U));
        lattice_y = -signed_ring;
    }

    constexpr double kSpawnSpacing = 12.0;
    return {
        .x = static_cast<float>(static_cast<double>(lattice_x) * kSpawnSpacing),
        .y = static_cast<float>(static_cast<double>(lattice_y) * kSpawnSpacing),
    };
}

SafePlayerSpawnResult spawn_player_safely(World& world, PlayerOwnerId owner_id) {
    if (!world.has_available_entity_id()) {
        return SafePlayerSpawnError::EntityIdExhausted;
    }

    auto ordinal = static_cast<std::uint64_t>(world.player_count());
    const auto start_ordinal = ordinal;
    while (true) {
        const auto candidate = initial_player_spawn_candidate(ordinal);
        const auto status = world.classify_initial_player_spawn(candidate);
        if (status == InitialPlayerSpawnStatus::OutsideRepresentableGrid) {
            if (start_ordinal == 0 || ordinal < start_ordinal) {
                return SafePlayerSpawnError::NoSafePosition;
            }
            ordinal = 0;
            continue;
        }

        if (status == InitialPlayerSpawnStatus::Clear) {
            const auto player = world.spawn_player(owner_id, candidate);
            return player ? SafePlayerSpawnResult{*player}
                          : SafePlayerSpawnResult{SafePlayerSpawnError::EntityIdExhausted};
        }

        if (ordinal < start_ordinal && ordinal + 1U == start_ordinal) {
            return SafePlayerSpawnError::NoSafePosition;
        }
        if (ordinal == std::numeric_limits<std::uint64_t>::max()) {
            if (start_ordinal == 0) {
                return SafePlayerSpawnError::NoSafePosition;
            }
            ordinal = 0;
            continue;
        }
        ++ordinal;
        if (ordinal == start_ordinal) {
            return SafePlayerSpawnError::NoSafePosition;
        }
    }
}

} // namespace dots::simulation

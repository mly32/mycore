#include "dots/simulation/spatial_grid.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <stdexcept>

TEST_CASE("Spatial grid tracks insertion, movement, removal, and broad-phase queries",
          "[dots][simulation][grid]") {
    dots::simulation::SpatialGrid grid{10.0F};
    const dots::simulation::EntityId first{1};
    const dots::simulation::EntityId second{2};

    REQUIRE(grid.insert(first, {.center = {2.0F, 2.0F}, .radius = 1.0F}));
    REQUIRE(grid.insert(second, {.center = {12.0F, 2.0F}, .radius = 3.0F}));
    REQUIRE(grid.entity_count() == 2);

    auto candidates = grid.query({.center = {8.0F, 2.0F}, .radius = 1.0F});
    REQUIRE(std::find(candidates.begin(), candidates.end(), first) != candidates.end());
    REQUIRE(std::find(candidates.begin(), candidates.end(), second) != candidates.end());

    REQUIRE(grid.update(first, {.center = {32.0F, 2.0F}, .radius = 1.0F}));
    candidates = grid.query({.center = {2.0F, 2.0F}, .radius = 1.0F});
    REQUIRE(std::find(candidates.begin(), candidates.end(), first) == candidates.end());
    candidates = grid.query({.center = {32.0F, 2.0F}, .radius = 1.0F});
    REQUIRE(std::find(candidates.begin(), candidates.end(), first) != candidates.end());

    REQUIRE(grid.remove(first));
    REQUIRE_FALSE(grid.contains(first));
    REQUIRE(grid.entity_count() == 1);
    REQUIRE_FALSE(grid.remove(first));
}

TEST_CASE("Spatial grid deduplicates multi-cell entities and handles negative cells",
          "[dots][simulation][grid]") {
    dots::simulation::SpatialGrid grid{10.0F};
    const dots::simulation::EntityId multi_cell{3};
    const dots::simulation::EntityId negative{4};
    REQUIRE(grid.insert(multi_cell, {.center = {10.0F, 0.0F}, .radius = 2.0F}));
    REQUIRE(grid.insert(negative, {.center = {-15.0F, -15.0F}, .radius = 1.0F}));

    auto candidates = grid.query({.center = {10.0F, 0.0F}, .radius = 2.0F});
    REQUIRE(std::count(candidates.begin(), candidates.end(), multi_cell) == 1);
    candidates = grid.query({.center = {-15.0F, -15.0F}, .radius = 1.0F});
    REQUIRE(std::count(candidates.begin(), candidates.end(), negative) == 1);

    REQUIRE(grid.update(negative, {.center = {-12.0F, -12.0F}, .radius = 1.0F}));
    REQUIRE(grid.entity_count() == 2);
}

TEST_CASE("Spatial grid rejects invalid configuration, IDs, and ranges",
          "[dots][simulation][grid][validation]") {
    REQUIRE_THROWS_AS(dots::simulation::SpatialGrid{0.0F}, std::invalid_argument);
    REQUIRE_THROWS_AS(dots::simulation::SpatialGrid{std::numeric_limits<float>::quiet_NaN()},
                      std::invalid_argument);

    dots::simulation::SpatialGrid grid{4.0F};
    const dots::simulation::EntityId entity{7};
    const auto huge = std::numeric_limits<float>::max();

    REQUIRE_FALSE(
        grid.insert(dots::simulation::EntityId::invalid(), {.center = {}, .radius = 1.0F}));
    REQUIRE_FALSE(grid.insert(entity, {.center = {huge, 0.0F}, .radius = 1.0F}));
    REQUIRE(grid.insert(entity, {.center = {}, .radius = 1.0F}));
    REQUIRE_FALSE(grid.insert(entity, {.center = {4.0F, 0.0F}, .radius = 1.0F}));
    REQUIRE_FALSE(grid.update(dots::simulation::EntityId{99}, {.center = {}, .radius = 1.0F}));
    REQUIRE_FALSE(grid.can_index({.center = {}, .radius = huge}));
    REQUIRE(grid.query({.center = {}, .radius = huge}).empty());
}

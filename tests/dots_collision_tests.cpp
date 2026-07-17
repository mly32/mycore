#include "dots/simulation/collision.hpp"

#include <catch2/catch_test_macros.hpp>
#include <limits>

TEST_CASE("Circle overlap handles touching, separation, and invalid geometry",
          "[dots][simulation][collision]") {
    using dots::simulation::Circle;
    using dots::simulation::circles_overlap;

    REQUIRE(circles_overlap(Circle{.center = {}, .radius = 1.0F},
                            Circle{.center = {3.0F, 0.0F}, .radius = 2.0F}));
    REQUIRE_FALSE(circles_overlap(Circle{.center = {}, .radius = 1.0F},
                                  Circle{.center = {3.01F, 0.0F}, .radius = 2.0F}));
    REQUIRE(circles_overlap(Circle{.center = {}, .radius = 0.0F},
                            Circle{.center = {}, .radius = 0.0F}));
    REQUIRE_FALSE(circles_overlap(Circle{.center = {}, .radius = -1.0F},
                                  Circle{.center = {}, .radius = 1.0F}));
    REQUIRE_FALSE(circles_overlap(
        Circle{.center = {std::numeric_limits<float>::quiet_NaN(), 0.0F}, .radius = 1.0F},
        Circle{.center = {}, .radius = 1.0F}));
}

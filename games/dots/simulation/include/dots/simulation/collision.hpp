#pragma once

#include "mycore/math/vector2.hpp"

#include <cmath>

namespace dots::simulation {

struct Circle {
    mycore::math::Vector2 center;
    float radius{};
};

[[nodiscard]] inline bool is_valid(Circle circle) noexcept {
    return std::isfinite(circle.center.x) && std::isfinite(circle.center.y) &&
           std::isfinite(circle.radius) && circle.radius >= 0.0F;
}

// Touching circles overlap. Invalid circles never overlap.
[[nodiscard]] inline bool circles_overlap(Circle lhs, Circle rhs) noexcept {
    if (!is_valid(lhs) || !is_valid(rhs)) {
        return false;
    }

    const auto delta_x = static_cast<double>(lhs.center.x) - static_cast<double>(rhs.center.x);
    const auto delta_y = static_cast<double>(lhs.center.y) - static_cast<double>(rhs.center.y);
    const auto radius_sum = static_cast<double>(lhs.radius) + static_cast<double>(rhs.radius);
    return (delta_x * delta_x) + (delta_y * delta_y) <= radius_sum * radius_sum;
}

} // namespace dots::simulation

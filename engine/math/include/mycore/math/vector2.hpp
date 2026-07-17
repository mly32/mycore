#pragma once

#include <cmath>

namespace mycore::math {

// A two-dimensional floating-point vector for positions and movement.
struct Vector2 {
    float x{};
    float y{};

    constexpr Vector2& operator+=(Vector2 rhs) noexcept {
        x += rhs.x;
        y += rhs.y;
        return *this;
    }

    constexpr Vector2& operator-=(Vector2 rhs) noexcept {
        x -= rhs.x;
        y -= rhs.y;
        return *this;
    }

    constexpr Vector2& operator*=(float scalar) noexcept {
        x *= scalar;
        y *= scalar;
        return *this;
    }

    constexpr Vector2& operator/=(float scalar) noexcept {
        x /= scalar;
        y /= scalar;
        return *this;
    }

    auto operator<=>(const Vector2&) const = default;
};

[[nodiscard]] constexpr Vector2 operator+(Vector2 lhs, Vector2 rhs) noexcept {
    return lhs += rhs;
}

[[nodiscard]] constexpr Vector2 operator-(Vector2 lhs, Vector2 rhs) noexcept {
    return lhs -= rhs;
}

[[nodiscard]] constexpr Vector2 operator-(Vector2 vector) noexcept {
    return {-vector.x, -vector.y};
}

[[nodiscard]] constexpr Vector2 operator*(Vector2 vector, float scalar) noexcept {
    return vector *= scalar;
}

[[nodiscard]] constexpr Vector2 operator*(float scalar, Vector2 vector) noexcept {
    return vector *= scalar;
}

[[nodiscard]] constexpr Vector2 operator/(Vector2 vector, float scalar) noexcept {
    return vector /= scalar;
}

[[nodiscard]] constexpr float dot(Vector2 lhs, Vector2 rhs) noexcept {
    return (lhs.x * rhs.x) + (lhs.y * rhs.y);
}

[[nodiscard]] constexpr float length_squared(Vector2 vector) noexcept {
    return dot(vector, vector);
}

[[nodiscard]] inline float length(Vector2 vector) noexcept {
    return std::sqrt(length_squared(vector));
}

[[nodiscard]] inline Vector2 normalized_or_zero(Vector2 vector) noexcept {
    const auto vector_length = length(vector);
    if (vector_length == 0.0F) {
        return {};
    }
    return vector / vector_length;
}

} // namespace mycore::math

#pragma once

#include <compare>
#include <concepts>
#include <limits>

namespace mycore::core {

// A type-safe identifier whose tag prevents mixing IDs from different domains.
template <typename Tag, std::unsigned_integral Representation> class StrongId {
public:
    using representation_type = Representation;

    static constexpr Representation kInvalidValue = std::numeric_limits<Representation>::max();

    constexpr StrongId() noexcept = default;
    explicit constexpr StrongId(Representation value) noexcept
        : value_(value) {}

    [[nodiscard]] static constexpr StrongId invalid() noexcept {
        return StrongId{};
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return value_ != kInvalidValue;
    }
    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return is_valid();
    }
    [[nodiscard]] constexpr Representation value() const noexcept {
        return value_;
    }

    auto operator<=>(const StrongId&) const = default;

private:
    Representation value_{kInvalidValue};
};

} // namespace mycore::core

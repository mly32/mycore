#pragma once

#include "dots/simulation/collision.hpp"
#include "dots/simulation/ids.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dots::simulation {

// A Dots-specific uniform grid. Entities are present in every cell touched by their bounds;
// queries return broad-phase candidates and callers perform exact collision checks.
class SpatialGrid {
public:
    enum class VisitResult : std::uint8_t {
        InvalidBounds,
        Completed,
        Stopped,
    };

    explicit SpatialGrid(float cell_size);

    [[nodiscard]] bool insert(EntityId entity_id, Circle bounds);
    [[nodiscard]] bool update(EntityId entity_id, Circle bounds);
    [[nodiscard]] bool remove(EntityId entity_id);

    [[nodiscard]] bool contains(EntityId entity_id) const noexcept;
    [[nodiscard]] bool can_index(Circle bounds) const noexcept;
    [[nodiscard]] std::size_t entity_count() const noexcept;
    [[nodiscard]] std::size_t occupied_cell_count() const noexcept;
    [[nodiscard]] std::vector<EntityId> query(Circle bounds) const;

    // Visits broad-phase IDs without allocating or deduplicating them. The visitor may receive
    // the same ID from multiple cells and returns false to stop traversal.
    template <typename Visitor>
    [[nodiscard]] VisitResult visit_candidates(Circle bounds, Visitor&& visitor) const;

private:
    struct Cell {
        std::int32_t x{};
        std::int32_t y{};

        auto operator<=>(const Cell&) const = default;
    };

    struct CellRange {
        Cell minimum;
        Cell maximum;

        bool operator==(const CellRange&) const = default;
    };

    [[nodiscard]] std::optional<CellRange> cell_range(Circle bounds) const noexcept;
    [[nodiscard]] static bool contains_cell(CellRange range, Cell cell) noexcept;
    void add_to_cells(EntityId entity_id,
                      CellRange range,
                      std::optional<CellRange> excluded_range = std::nullopt);
    void remove_from_cells(EntityId entity_id,
                           CellRange range,
                           std::optional<CellRange> excluded_range = std::nullopt) noexcept;

    float cell_size_;
    std::map<Cell, std::vector<EntityId>> cells_;
    std::unordered_map<std::uint32_t, CellRange> entries_by_id_;
};

template <typename Visitor>
SpatialGrid::VisitResult SpatialGrid::visit_candidates(Circle bounds, Visitor&& visitor) const {
    const auto range = cell_range(bounds);
    if (!range) {
        return VisitResult::InvalidBounds;
    }

    for (std::int64_t y = range->minimum.y; y <= range->maximum.y; ++y) {
        for (std::int64_t x = range->minimum.x; x <= range->maximum.x; ++x) {
            const auto cell =
                cells_.find({.x = static_cast<std::int32_t>(x), .y = static_cast<std::int32_t>(y)});
            if (cell == cells_.end()) {
                continue;
            }
            for (const auto entity_id : cell->second) {
                if (!visitor(entity_id)) {
                    return VisitResult::Stopped;
                }
            }
        }
    }
    return VisitResult::Completed;
}

} // namespace dots::simulation

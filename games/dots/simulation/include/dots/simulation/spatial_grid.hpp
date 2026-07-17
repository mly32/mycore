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
    explicit SpatialGrid(float cell_size);

    [[nodiscard]] bool insert(EntityId entity_id, Circle bounds);
    [[nodiscard]] bool update(EntityId entity_id, Circle bounds);
    [[nodiscard]] bool remove(EntityId entity_id);

    [[nodiscard]] bool contains(EntityId entity_id) const noexcept;
    [[nodiscard]] bool can_index(Circle bounds) const noexcept;
    [[nodiscard]] std::size_t entity_count() const noexcept;
    [[nodiscard]] std::vector<EntityId> query(Circle bounds) const;

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

} // namespace dots::simulation

#include "dots/simulation/spatial_grid.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace dots::simulation {
namespace {

constexpr std::int64_t kMaximumCellsPerAxis = 1'024;

} // namespace

SpatialGrid::SpatialGrid(float cell_size)
    : cell_size_(cell_size) {
    if (!std::isfinite(cell_size_) || cell_size_ <= 0.0F) {
        throw std::invalid_argument{"Dots spatial-grid cell size must be finite and positive"};
    }
}

bool SpatialGrid::insert(EntityId entity_id, Circle bounds) {
    const auto range = cell_range(bounds);
    if (!entity_id.is_valid() || !range || contains(entity_id)) {
        return false;
    }

    const auto [entry, inserted] = entries_by_id_.emplace(entity_id.value(), *range);
    if (!inserted) {
        return false;
    }

    try {
        add_to_cells(entity_id, *range);
    } catch (...) {
        remove_from_cells(entity_id, *range);
        entries_by_id_.erase(entry);
        throw;
    }
    return true;
}

bool SpatialGrid::update(EntityId entity_id, Circle bounds) {
    const auto range = cell_range(bounds);
    if (!entity_id.is_valid() || !range) {
        return false;
    }

    auto entry = entries_by_id_.find(entity_id.value());
    if (entry == entries_by_id_.end()) {
        return false;
    }
    if (entry->second == *range) {
        return true;
    }

    const auto old_range = entry->second;
    try {
        add_to_cells(entity_id, *range, old_range);
    } catch (...) {
        remove_from_cells(entity_id, *range, old_range);
        throw;
    }
    remove_from_cells(entity_id, old_range, *range);
    entry->second = *range;
    return true;
}

bool SpatialGrid::remove(EntityId entity_id) {
    if (!entity_id.is_valid()) {
        return false;
    }

    auto entry = entries_by_id_.find(entity_id.value());
    if (entry == entries_by_id_.end()) {
        return false;
    }

    remove_from_cells(entity_id, entry->second);
    entries_by_id_.erase(entry);
    return true;
}

bool SpatialGrid::contains(EntityId entity_id) const noexcept {
    return entity_id.is_valid() && entries_by_id_.contains(entity_id.value());
}

bool SpatialGrid::can_index(Circle bounds) const noexcept {
    return cell_range(bounds).has_value();
}

std::size_t SpatialGrid::entity_count() const noexcept {
    return entries_by_id_.size();
}

std::vector<EntityId> SpatialGrid::query(Circle bounds) const {
    const auto range = cell_range(bounds);
    if (!range) {
        return {};
    }

    std::vector<EntityId> result;
    std::unordered_set<std::uint32_t> seen;
    for (std::int64_t y = range->minimum.y; y <= range->maximum.y; ++y) {
        for (std::int64_t x = range->minimum.x; x <= range->maximum.x; ++x) {
            const auto cell =
                cells_.find({.x = static_cast<std::int32_t>(x), .y = static_cast<std::int32_t>(y)});
            if (cell == cells_.end()) {
                continue;
            }
            for (const auto entity_id : cell->second) {
                if (seen.insert(entity_id.value()).second) {
                    result.push_back(entity_id);
                }
            }
        }
    }
    return result;
}

std::optional<SpatialGrid::CellRange> SpatialGrid::cell_range(Circle bounds) const noexcept {
    if (!is_valid(bounds)) {
        return std::nullopt;
    }

    const auto minimum_x =
        std::floor((static_cast<double>(bounds.center.x) - static_cast<double>(bounds.radius)) /
                   static_cast<double>(cell_size_));
    const auto minimum_y =
        std::floor((static_cast<double>(bounds.center.y) - static_cast<double>(bounds.radius)) /
                   static_cast<double>(cell_size_));
    const auto maximum_x =
        std::floor((static_cast<double>(bounds.center.x) + static_cast<double>(bounds.radius)) /
                   static_cast<double>(cell_size_));
    const auto maximum_y =
        std::floor((static_cast<double>(bounds.center.y) + static_cast<double>(bounds.radius)) /
                   static_cast<double>(cell_size_));

    constexpr auto kCellMinimum = static_cast<double>(std::numeric_limits<std::int32_t>::min());
    constexpr auto kCellMaximum = static_cast<double>(std::numeric_limits<std::int32_t>::max());
    if (minimum_x < kCellMinimum || minimum_y < kCellMinimum || maximum_x > kCellMaximum ||
        maximum_y > kCellMaximum || maximum_x - minimum_x + 1.0 > kMaximumCellsPerAxis ||
        maximum_y - minimum_y + 1.0 > kMaximumCellsPerAxis) {
        return std::nullopt;
    }

    return CellRange{
        .minimum = {.x = static_cast<std::int32_t>(minimum_x),
                    .y = static_cast<std::int32_t>(minimum_y)},
        .maximum = {.x = static_cast<std::int32_t>(maximum_x),
                    .y = static_cast<std::int32_t>(maximum_y)},
    };
}

bool SpatialGrid::contains_cell(CellRange range, Cell cell) noexcept {
    return cell.x >= range.minimum.x && cell.x <= range.maximum.x && cell.y >= range.minimum.y &&
           cell.y <= range.maximum.y;
}

void SpatialGrid::add_to_cells(EntityId entity_id,
                               CellRange range,
                               std::optional<CellRange> excluded_range) {
    for (std::int64_t y = range.minimum.y; y <= range.maximum.y; ++y) {
        for (std::int64_t x = range.minimum.x; x <= range.maximum.x; ++x) {
            const Cell cell{.x = static_cast<std::int32_t>(x), .y = static_cast<std::int32_t>(y)};
            if (!excluded_range || !contains_cell(*excluded_range, cell)) {
                cells_[cell].push_back(entity_id);
            }
        }
    }
}

void SpatialGrid::remove_from_cells(EntityId entity_id,
                                    CellRange range,
                                    std::optional<CellRange> excluded_range) noexcept {
    for (std::int64_t y = range.minimum.y; y <= range.maximum.y; ++y) {
        for (std::int64_t x = range.minimum.x; x <= range.maximum.x; ++x) {
            const Cell key{.x = static_cast<std::int32_t>(x), .y = static_cast<std::int32_t>(y)};
            if (excluded_range && contains_cell(*excluded_range, key)) {
                continue;
            }
            auto cell = cells_.find(key);
            if (cell == cells_.end()) {
                continue;
            }
            std::erase(cell->second, entity_id);
            if (cell->second.empty()) {
                cells_.erase(cell);
            }
        }
    }
}

} // namespace dots::simulation

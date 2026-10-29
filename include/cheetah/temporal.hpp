#pragma once

#include "cheetah/types.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace cheetah {

struct TimeWindow {
    Timestamp start{};
    Timestamp end{};

    // Windows are half-open: [start, end). This prevents a boundary point
    // from being copied into two consecutive non-overlapping windows.
    bool contains(Timestamp timestamp) const noexcept {
        return timestamp >= start && timestamp < end;
    }
};

struct TemporalFilterOptions {
    std::size_t minimum_points{2};
};

std::optional<std::pair<Timestamp, Timestamp>> dataset_time_bounds(
    const Dataset& dataset);

std::vector<TimeWindow> generate_time_windows(Timestamp start,
                                              Timestamp end,
                                              Timestamp window_size,
                                              Timestamp step);

Dataset filter_by_time_window(
    const Dataset& dataset,
    const TimeWindow& window,
    const TemporalFilterOptions& options = {});

}  // namespace cheetah

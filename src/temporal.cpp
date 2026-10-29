#include "cheetah/temporal.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace cheetah {

std::optional<std::pair<Timestamp, Timestamp>> dataset_time_bounds(
    const Dataset& dataset) {
    Timestamp minimum = std::numeric_limits<Timestamp>::max();
    Timestamp maximum = std::numeric_limits<Timestamp>::min();
    bool found = false;
    for (const auto& trajectory : dataset.trajectories) {
        for (const auto& location : trajectory.locations) {
            if (!location.has_timestamp()) {
                continue;
            }
            found = true;
            minimum = std::min(minimum, location.timestamp);
            maximum = std::max(maximum, location.timestamp);
        }
    }
    if (!found) {
        return std::nullopt;
    }
    if (maximum == std::numeric_limits<Timestamp>::max()) {
        throw std::overflow_error(
            "El timestamp maximo no puede convertirse en limite exclusivo");
    }
    // The second value is exclusive, matching TimeWindow::contains.
    return std::make_pair(minimum, maximum + 1);
}

std::vector<TimeWindow> generate_time_windows(Timestamp start,
                                              Timestamp end,
                                              Timestamp window_size,
                                              Timestamp step) {
    if (end < start) {
        throw std::invalid_argument("El final temporal no puede preceder al inicio");
    }
    if (window_size <= 0 || step <= 0) {
        throw std::invalid_argument("El tamano y paso de ventana deben ser positivos");
    }

    std::vector<TimeWindow> windows;
    for (Timestamp cursor = start; cursor < end;) {
        const Timestamp remaining = end - cursor;
        const Timestamp window_end =
            remaining < window_size ? end : cursor + window_size;
        windows.push_back({cursor, window_end});
        if (step >= remaining) {
            break;
        }
        cursor += step;
    }
    return windows;
}

Dataset filter_by_time_window(const Dataset& dataset,
                              const TimeWindow& window,
                              const TemporalFilterOptions& options) {
    if (window.end <= window.start) {
        throw std::invalid_argument("La ventana temporal debe tener duracion positiva");
    }

    Dataset filtered;
    filtered.source_path = dataset.source_path;
    for (const auto& trajectory : dataset.trajectories) {
        Trajectory segment;
        segment.source_id = trajectory.source_id >= 0
            ? trajectory.source_id
            : trajectory.id;
        std::copy_if(
            trajectory.locations.begin(), trajectory.locations.end(),
            std::back_inserter(segment.locations),
            [&](const Location& location) {
                return location.has_timestamp() &&
                       window.contains(location.timestamp);
            });
        if (segment.locations.size() < options.minimum_points) {
            continue;
        }
        segment.id = static_cast<int>(filtered.trajectories.size());
        filtered.trajectories.push_back(std::move(segment));
    }
    return filtered;
}

}  // namespace cheetah

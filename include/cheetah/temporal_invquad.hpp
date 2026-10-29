#pragma once

#include "cheetah/projection.hpp"
#include "cheetah/quadtree.hpp"
#include "cheetah/temporal.hpp"
#include "cheetah/types.hpp"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace cheetah {

struct TemporalIndexOptions {
    Timestamp window_size{3600};
    Timestamp window_step{3600};
    std::size_t minimum_points{2};
    int height{3};
    int delta{4};
    ProjectionOptions projection;
};

struct TemporalIndexBuildResult {
    std::size_t windows{};
    std::size_t nonempty_windows{};
    std::size_t stored_segments{};
};

struct TemporalQueryResult {
    TimeWindow window;
    std::size_t available_trajectories{};
    std::size_t indexed_selection_count{};
    std::size_t supplemented_count{};
    double quality{};
    bool target_met{};
    std::vector<std::int64_t> source_ids;
};

class TemporalInvQuadIndex {
public:
    static TemporalIndexBuildResult build(
        const Dataset& complete,
        const std::filesystem::path& output,
        const TemporalIndexOptions& options);

    static TemporalQueryResult query(
        const std::filesystem::path& index_directory,
        std::size_t window_index,
        const GeoRect& region,
        double quality_target);
};

}  // namespace cheetah

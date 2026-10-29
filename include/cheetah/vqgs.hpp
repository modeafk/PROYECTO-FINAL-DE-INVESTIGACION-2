#pragma once

#include "cheetah/types.hpp"
#include "cheetah/projection.hpp"

#include <cstddef>
#include <unordered_set>
#include <vector>

namespace cheetah {

using PixelSet = std::unordered_set<Pixel, PixelHash>;

struct VqgsOptions {
    int delta{64};
    double sampling_rate{0.005};
    std::size_t sample_count{0};
    // Zero uses the fixed-budget mode. A value in (0, 1] makes the greedy
    // selection stop as soon as cumulative visual quality reaches the target.
    double quality_target{0.0};
};

struct VqgsResult {
    std::vector<int> trajectory_ids;
    PixelSet augmented_coverage;
    std::vector<int> marginal_scores;
    std::vector<double> cumulative_quality;
};

class VqgsEngine {
public:
    explicit VqgsEngine(const Dataset& dataset);

    VqgsResult select(const VqgsOptions& options) const;
    std::vector<int> representative_scores(const VqgsResult& result,
                                           int delta) const;
    double quality(const VqgsResult& result) const;

    const PixelSet& all_pixels() const noexcept { return all_pixels_; }

private:
    const Dataset& dataset_;
    PixelSet all_pixels_;

    int marginal_score(const Trajectory& trajectory,
                       int delta,
                       const PixelSet& covered) const;
    void update_coverage(PixelSet& covered,
                         const Trajectory& trajectory,
                         int delta) const;
};

// Reprojects the original locations and evaluates a fixed selection at a
// requested zoom, matching the separate Quality stage in the Java project.
double quality_at_projection(const Dataset& dataset,
                             const std::vector<int>& trajectory_ids,
                             const WebMercatorProjection& projection,
                             int delta);

}  // namespace cheetah

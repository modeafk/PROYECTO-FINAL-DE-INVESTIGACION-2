#include "cheetah/vqgs.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>

namespace cheetah {
namespace {

struct HeapEntry {
    int score{};
    int trajectory_id{};
    std::size_t version{};
};

struct HeapOrder {
    bool operator()(const HeapEntry& left, const HeapEntry& right) const noexcept {
        if (left.score != right.score) {
            return left.score < right.score;
        }
        return left.trajectory_id > right.trajectory_id;
    }
};

}  // namespace

VqgsEngine::VqgsEngine(const Dataset& dataset) : dataset_(dataset) {
    for (const auto& trajectory : dataset_.trajectories) {
        all_pixels_.insert(trajectory.pixels.begin(), trajectory.pixels.end());
    }
}

int VqgsEngine::marginal_score(const Trajectory& trajectory,
                               int delta,
                               const PixelSet& covered) const {
    int score = 0;
    for (const auto& pixel : trajectory.pixels) {
        bool already_covered = false;
        for (int dx = -delta; dx <= delta && !already_covered; ++dx) {
            for (int dy = -delta; dy <= delta; ++dy) {
                if (covered.find({pixel.x + dx, pixel.y + dy}) != covered.end()) {
                    already_covered = true;
                    break;
                }
            }
        }
        if (!already_covered) {
            ++score;
        }
    }
    return score;
}

void VqgsEngine::update_coverage(PixelSet& covered,
                                 const Trajectory& trajectory,
                                 int delta) const {
    for (const auto& pixel : trajectory.pixels) {
        for (int dx = -delta; dx <= delta; ++dx) {
            for (int dy = -delta; dy <= delta; ++dy) {
                Pixel candidate{pixel.x + dx, pixel.y + dy};
                if (all_pixels_.find(candidate) != all_pixels_.end()) {
                    covered.insert(candidate);
                }
            }
        }
    }
}

VqgsResult VqgsEngine::select(const VqgsOptions& options) const {
    if (options.delta < 0) {
        throw std::invalid_argument("delta no puede ser negativo");
    }
    if (options.sampling_rate < 0.0 || options.sampling_rate > 1.0) {
        throw std::invalid_argument("sampling_rate debe estar entre 0 y 1");
    }
    if (options.quality_target < 0.0 || options.quality_target > 1.0) {
        throw std::invalid_argument("quality_target debe estar entre 0 y 1");
    }

    const std::size_t trajectory_count = dataset_.trajectories.size();
    const bool quality_mode = options.quality_target > 0.0;
    std::size_t target_count = quality_mode
        ? trajectory_count
        : options.sample_count;
    if (!quality_mode && target_count == 0) {
        target_count = static_cast<std::size_t>(
            std::ceil(static_cast<double>(trajectory_count) *
                      options.sampling_rate));
    }
    target_count = std::min(target_count, trajectory_count);

    VqgsResult result;
    result.trajectory_ids.reserve(target_count);
    result.marginal_scores.reserve(target_count);
    result.cumulative_quality.reserve(target_count);
    if (target_count == 0) {
        return result;
    }

    std::priority_queue<HeapEntry, std::vector<HeapEntry>, HeapOrder> heap;
    std::vector<std::size_t> versions(trajectory_count, 0);
    std::vector<unsigned char> selected(trajectory_count, 0);

    for (const auto& trajectory : dataset_.trajectories) {
        heap.push({trajectory.original_score, trajectory.id, 0});
    }

    auto discard_stale = [&]() {
        while (!heap.empty()) {
            const auto& entry = heap.top();
            if (selected[entry.trajectory_id] != 0 ||
                entry.version != versions[entry.trajectory_id]) {
                heap.pop();
            } else {
                break;
            }
        }
    };

    while (result.trajectory_ids.size() < target_count) {
        discard_stale();
        if (heap.empty()) {
            break;
        }

        HeapEntry candidate = heap.top();
        heap.pop();
        const auto& trajectory = dataset_.trajectories[candidate.trajectory_id];
        const int actual_score =
            marginal_score(trajectory, options.delta, result.augmented_coverage);

        discard_stale();
        const int next_upper_bound =
            heap.empty() ? std::numeric_limits<int>::min() : heap.top().score;

        if (actual_score >= next_upper_bound) {
            selected[candidate.trajectory_id] = 1;
            result.trajectory_ids.push_back(candidate.trajectory_id);
            result.marginal_scores.push_back(actual_score);
            update_coverage(result.augmented_coverage, trajectory, options.delta);
            result.cumulative_quality.push_back(
                all_pixels_.empty()
                    ? 1.0
                    : static_cast<double>(result.augmented_coverage.size()) /
                          static_cast<double>(all_pixels_.size()));
            if (quality_mode &&
                result.cumulative_quality.back() >= options.quality_target) {
                break;
            }
        } else {
            ++versions[candidate.trajectory_id];
            heap.push({actual_score, candidate.trajectory_id,
                       versions[candidate.trajectory_id]});
        }
    }

    return result;
}

std::vector<int> VqgsEngine::representative_scores(const VqgsResult& result,
                                                   int delta) const {
    std::vector<int> scores(result.trajectory_ids.size(), 0);
    if (result.trajectory_ids.empty()) {
        return scores;
    }

    std::unordered_map<Pixel, std::vector<std::size_t>, PixelHash> pixel_to_samples;
    for (std::size_t sample_index = 0;
         sample_index < result.trajectory_ids.size(); ++sample_index) {
        const auto& trajectory = dataset_.trajectories[result.trajectory_ids[sample_index]];
        for (const auto& pixel : trajectory.pixels) {
            for (int dx = -delta; dx <= delta; ++dx) {
                for (int dy = -delta; dy <= delta; ++dy) {
                    Pixel candidate{pixel.x + dx, pixel.y + dy};
                    if (all_pixels_.find(candidate) != all_pixels_.end()) {
                        pixel_to_samples[candidate].push_back(sample_index);
                    }
                }
            }
        }
    }

    for (auto& item : pixel_to_samples) {
        auto& values = item.second;
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }

    std::vector<int> overlaps(result.trajectory_ids.size(), 0);
    for (const auto& trajectory : dataset_.trajectories) {
        std::fill(overlaps.begin(), overlaps.end(), 0);
        for (const auto& pixel : trajectory.pixels) {
            const auto found = pixel_to_samples.find(pixel);
            if (found == pixel_to_samples.end()) {
                continue;
            }
            for (std::size_t sample_index : found->second) {
                ++overlaps[sample_index];
            }
        }
        const auto best = std::max_element(overlaps.begin(), overlaps.end());
        ++scores[static_cast<std::size_t>(std::distance(overlaps.begin(), best))];
    }

    return scores;
}

double VqgsEngine::quality(const VqgsResult& result) const {
    if (all_pixels_.empty()) {
        return 1.0;
    }
    std::size_t covered = 0;
    for (const auto& pixel : all_pixels_) {
        if (result.augmented_coverage.find(pixel) != result.augmented_coverage.end()) {
            ++covered;
        }
    }
    return static_cast<double>(covered) / static_cast<double>(all_pixels_.size());
}

double quality_at_projection(const Dataset& dataset,
                             const std::vector<int>& trajectory_ids,
                             const WebMercatorProjection& projection,
                             int delta) {
    if (delta < 0) {
        throw std::invalid_argument("delta no puede ser negativo");
    }

    PixelSet all_pixels;
    std::vector<PixelSet> selected_pixels(trajectory_ids.size());
    std::vector<int> selected_lookup(dataset.trajectories.size(), -1);
    for (std::size_t index = 0; index < trajectory_ids.size(); ++index) {
        const int id = trajectory_ids[index];
        if (id < 0 || static_cast<std::size_t>(id) >= dataset.trajectories.size()) {
            throw std::out_of_range("ID de trayectoria fuera de rango");
        }
        selected_lookup[static_cast<std::size_t>(id)] = static_cast<int>(index);
    }

    for (const auto& trajectory : dataset.trajectories) {
        const int selected_index = selected_lookup[static_cast<std::size_t>(trajectory.id)];
        for (const auto& location : trajectory.locations) {
            const Pixel pixel = projection.project(location);
            all_pixels.insert(pixel);
            if (selected_index >= 0) {
                selected_pixels[static_cast<std::size_t>(selected_index)].insert(pixel);
            }
        }
    }

    if (all_pixels.empty()) {
        return 1.0;
    }

    PixelSet covered;
    for (const auto& pixels : selected_pixels) {
        for (const auto& pixel : pixels) {
            for (int dx = -delta; dx <= delta; ++dx) {
                for (int dy = -delta; dy <= delta; ++dy) {
                    Pixel candidate{pixel.x + dx, pixel.y + dy};
                    if (all_pixels.find(candidate) != all_pixels.end()) {
                        covered.insert(candidate);
                    }
                }
            }
        }
    }

    return static_cast<double>(covered.size()) /
           static_cast<double>(all_pixels.size());
}

}  // namespace cheetah

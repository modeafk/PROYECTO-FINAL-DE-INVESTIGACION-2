#pragma once

#include "cheetah/types.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace cheetah {

struct LoadOptions {
    std::size_t limit{0};
    // The Java loader accidentally skips the final longitude/latitude pair.
    // Enable this only when reproducing that historical behavior.
    bool java_loader_compatible{false};
};

Dataset load_dataset(const std::filesystem::path& path,
                     const LoadOptions& options = {});

// CSV row format:
// trajectory_id,longitude,latitude,timestamp
// timestamp is an integer expressed in Unix seconds.
Dataset load_temporal_dataset(const std::filesystem::path& path,
                              const LoadOptions& options = {});

struct PortoLoadOptions {
    // Maximum number of accepted trajectories. Zero means no limit and can
    // require substantial memory for the complete 1.94 GB train.csv.
    std::size_t limit{0};
    bool skip_missing_data{true};
    std::size_t minimum_points{2};
    Timestamp point_interval_seconds{15};
    std::optional<Timestamp> start_time;
    std::optional<Timestamp> end_time;
};

// Reads the original ECML/PKDD Porto train.csv. TIMESTAMP is the trip start;
// point i receives TIMESTAMP + i * point_interval_seconds. When start/end are
// present, only points in the half-open interval [start, end) are retained.
Dataset load_porto_train_dataset(const std::filesystem::path& path,
                                 const PortoLoadOptions& options = {});

void save_positions(const Dataset& dataset, const std::filesystem::path& path);
void save_selection(const std::vector<int>& trajectory_ids,
                    const std::filesystem::path& path);
void save_representativeness(const std::vector<int>& trajectory_ids,
                             const std::vector<int>& scores,
                             const std::filesystem::path& path);

}  // namespace cheetah

#include "cheetah/projection.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace cheetah {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxMercatorLatitude = 85.05112878;
}

WebMercatorProjection::WebMercatorProjection(ProjectionOptions options)
    : options_(options),
      world_size_(256.0 * std::pow(2.0, options_.zoom)) {
    center_world_x_ = normalized_x(options_.center_longitude) * world_size_;
    center_world_y_ = normalized_y(options_.center_latitude) * world_size_;
}

double WebMercatorProjection::clamp_latitude(double latitude) {
    return std::clamp(latitude, -kMaxMercatorLatitude, kMaxMercatorLatitude);
}

double WebMercatorProjection::normalized_x(double longitude) {
    return (longitude + 180.0) / 360.0;
}

double WebMercatorProjection::normalized_y(double latitude) {
    const double lat_rad = clamp_latitude(latitude) * kPi / 180.0;
    return (1.0 - std::asinh(std::tan(lat_rad)) / kPi) / 2.0;
}

Pixel WebMercatorProjection::project(const Location& location) const {
    const double world_x = normalized_x(location.longitude) * world_size_;
    const double world_y = normalized_y(location.latitude) * world_size_;
    return {
        static_cast<int>(world_x - center_world_x_ + options_.viewport_width / 2.0),
        static_cast<int>(world_y - center_world_y_ + options_.viewport_height / 2.0)
    };
}

void rasterize_dataset(Dataset& dataset, const WebMercatorProjection& projection) {
    for (auto& trajectory : dataset.trajectories) {
        std::unordered_set<Pixel, PixelHash> unique_pixels;
        unique_pixels.reserve(trajectory.locations.size());
        for (const auto& location : trajectory.locations) {
            unique_pixels.insert(projection.project(location));
        }
        trajectory.pixels.assign(unique_pixels.begin(), unique_pixels.end());
        std::sort(trajectory.pixels.begin(), trajectory.pixels.end());
        trajectory.original_score = static_cast<int>(trajectory.pixels.size());
    }
}

}  // namespace cheetah


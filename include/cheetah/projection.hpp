#pragma once

#include "cheetah/types.hpp"

#include <vector>

namespace cheetah {

struct ProjectionOptions {
    int zoom{20};
    double center_latitude{42.0273551940919};
    double center_longitude{-9.179738998413087};
    int viewport_width{1200};
    int viewport_height{800};
};

class WebMercatorProjection {
public:
    explicit WebMercatorProjection(ProjectionOptions options);

    Pixel project(const Location& location) const;
    const ProjectionOptions& options() const noexcept { return options_; }

private:
    ProjectionOptions options_;
    double center_world_x_{};
    double center_world_y_{};
    double world_size_{};

    static double clamp_latitude(double latitude);
    static double normalized_x(double longitude);
    static double normalized_y(double latitude);
};

void rasterize_dataset(Dataset& dataset, const WebMercatorProjection& projection);

}  // namespace cheetah


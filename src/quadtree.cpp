#include "cheetah/quadtree.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace cheetah {

bool GeoRect::contains(const Location& location) const noexcept {
    return location.latitude >= min_latitude && location.latitude <= max_latitude &&
           location.longitude >= min_longitude && location.longitude <= max_longitude;
}

bool GeoRect::intersects(const GeoRect& other) const noexcept {
    return !(max_latitude < other.min_latitude || min_latitude > other.max_latitude ||
             max_longitude < other.min_longitude || min_longitude > other.max_longitude);
}

QuadTreeIndex::QuadTreeIndex(const Dataset& dataset, int height) : dataset_(dataset) {
    if (height < 1) {
        throw std::invalid_argument("La altura del quad-tree debe ser al menos 1");
    }
    GeoRect bounds{
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest()
    };
    std::vector<int> ids;
    ids.reserve(dataset_.trajectories.size());
    for (const auto& trajectory : dataset_.trajectories) {
        ids.push_back(trajectory.id);
        for (const auto& location : trajectory.locations) {
            bounds.min_latitude = std::min(bounds.min_latitude, location.latitude);
            bounds.max_latitude = std::max(bounds.max_latitude, location.latitude);
            bounds.min_longitude = std::min(bounds.min_longitude, location.longitude);
            bounds.max_longitude = std::max(bounds.max_longitude, location.longitude);
        }
    }
    if (ids.empty()) {
        bounds = {};
    }
    root_ = build(bounds, ids, height);
}

std::unique_ptr<QuadTreeIndex::Node> QuadTreeIndex::build(
    const GeoRect& bounds,
    const std::vector<int>& trajectory_ids,
    int remaining_height) const {
    auto node = std::make_unique<Node>();
    node->bounds = bounds;
    node->trajectory_ids = trajectory_ids;
    if (remaining_height <= 1 || trajectory_ids.empty()) {
        return node;
    }

    const double mid_lat = (bounds.min_latitude + bounds.max_latitude) / 2.0;
    const double mid_lon = (bounds.min_longitude + bounds.max_longitude) / 2.0;
    const GeoRect children_bounds[4] = {
        {bounds.min_latitude, mid_lat, bounds.min_longitude, mid_lon},
        {bounds.min_latitude, mid_lat, mid_lon, bounds.max_longitude},
        {mid_lat, bounds.max_latitude, bounds.min_longitude, mid_lon},
        {mid_lat, bounds.max_latitude, mid_lon, bounds.max_longitude}
    };

    node->children.reserve(4);
    for (const auto& child_bounds : children_bounds) {
        std::vector<int> child_ids;
        for (int id : trajectory_ids) {
            if (trajectory_intersects(id, child_bounds)) {
                child_ids.push_back(id);
            }
        }
        node->children.push_back(build(child_bounds, child_ids, remaining_height - 1));
    }
    return node;
}

bool QuadTreeIndex::trajectory_intersects(int trajectory_id,
                                          const GeoRect& region) const {
    const auto& trajectory = dataset_.trajectories[trajectory_id];
    return std::any_of(trajectory.locations.begin(), trajectory.locations.end(),
                       [&](const Location& location) { return region.contains(location); });
}

void QuadTreeIndex::query_node(const Node& node,
                               const GeoRect& region,
                               std::vector<int>& result,
                               std::vector<unsigned char>& seen) const {
    if (!node.bounds.intersects(region)) {
        return;
    }
    if (node.children.empty()) {
        for (int id : node.trajectory_ids) {
            if (seen[id] == 0 && trajectory_intersects(id, region)) {
                seen[id] = 1;
                result.push_back(id);
            }
        }
        return;
    }
    for (const auto& child : node.children) {
        query_node(*child, region, result, seen);
    }
}

std::vector<int> QuadTreeIndex::query(const GeoRect& region) const {
    std::vector<int> result;
    std::vector<unsigned char> seen(dataset_.trajectories.size(), 0);
    query_node(*root_, region, result, seen);
    return result;
}

const GeoRect& QuadTreeIndex::bounds() const noexcept {
    return root_->bounds;
}

}  // namespace cheetah


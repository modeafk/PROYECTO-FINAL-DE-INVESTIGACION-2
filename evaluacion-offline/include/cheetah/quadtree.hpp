#pragma once

#include "cheetah/types.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace cheetah {

struct GeoRect {
    double min_latitude{};
    double max_latitude{};
    double min_longitude{};
    double max_longitude{};

    bool contains(const Location& location) const noexcept;
    bool intersects(const GeoRect& other) const noexcept;
};

class QuadTreeIndex {
public:
    QuadTreeIndex(const Dataset& dataset, int height);

    std::vector<int> query(const GeoRect& region) const;
    const GeoRect& bounds() const noexcept;

private:
    struct Node {
        GeoRect bounds;
        std::vector<int> trajectory_ids;
        std::vector<std::unique_ptr<Node>> children;
    };

    const Dataset& dataset_;
    std::unique_ptr<Node> root_;

    std::unique_ptr<Node> build(const GeoRect& bounds,
                                const std::vector<int>& trajectory_ids,
                                int remaining_height) const;
    void query_node(const Node& node,
                    const GeoRect& region,
                    std::vector<int>& result,
                    std::vector<unsigned char>& seen) const;
    bool trajectory_intersects(int trajectory_id, const GeoRect& region) const;
};

}  // namespace cheetah


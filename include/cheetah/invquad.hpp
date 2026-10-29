#pragma once

#include "cheetah/projection.hpp"
#include "cheetah/quadtree.hpp"
#include "cheetah/types.hpp"

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace cheetah {

struct InvQuadEntry {
    int trajectory_id{};
    double cumulative_quality{};
};

class InvQuadIndex {
public:
    InvQuadIndex(const Dataset& dataset,
                 const WebMercatorProjection& projection,
                 int height,
                 int delta);

    std::vector<int> query(const GeoRect& region, double quality) const;
    void save(const std::filesystem::path& path) const;
    static std::unique_ptr<InvQuadIndex> load(
        const Dataset& dataset,
        const WebMercatorProjection& projection,
        const std::filesystem::path& path);

    const GeoRect& bounds() const noexcept;
    int height() const noexcept { return height_; }
    int delta() const noexcept { return delta_; }

private:
    struct Node {
        GeoRect bounds;
        std::vector<InvQuadEntry> entries;
        std::vector<std::unique_ptr<Node>> children;
    };

    const Dataset& dataset_;
    const WebMercatorProjection& projection_;
    int height_{};
    int delta_{};
    std::unique_ptr<Node> root_;

    InvQuadIndex(const Dataset& dataset,
                 const WebMercatorProjection& projection,
                 int height,
                 int delta,
                 bool build_now);

    std::unique_ptr<Node> build(const GeoRect& bounds,
                                const std::vector<int>& candidate_ids,
                                int remaining_height) const;
    std::vector<InvQuadEntry> build_entries(const GeoRect& bounds,
                                            const std::vector<int>& ids) const;
    bool trajectory_intersects(int id, const GeoRect& region) const;
    void query_node(const Node& node,
                    const GeoRect& region,
                    double quality,
                    std::vector<int>& result,
                    std::vector<unsigned char>& seen) const;
    void save_node(const Node& node,
                   const std::string& path,
                   std::ostream& output) const;
};

}  // namespace cheetah

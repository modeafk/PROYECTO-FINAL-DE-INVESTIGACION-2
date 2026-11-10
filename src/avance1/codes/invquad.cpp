#include "cheetah/invquad.hpp"

#include "cheetah/vqgs.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>

namespace cheetah {
namespace {

bool rect_contains(const GeoRect& outer, const GeoRect& inner) {
    return outer.min_latitude <= inner.min_latitude &&
           outer.max_latitude >= inner.max_latitude &&
           outer.min_longitude <= inner.min_longitude &&
           outer.max_longitude >= inner.max_longitude;
}

GeoRect dataset_bounds(const Dataset& dataset) {
    GeoRect bounds{
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest()
    };
    bool found = false;
    for (const auto& trajectory : dataset.trajectories) {
        for (const auto& location : trajectory.locations) {
            found = true;
            bounds.min_latitude = std::min(bounds.min_latitude, location.latitude);
            bounds.max_latitude = std::max(bounds.max_latitude, location.latitude);
            bounds.min_longitude = std::min(bounds.min_longitude, location.longitude);
            bounds.max_longitude = std::max(bounds.max_longitude, location.longitude);
        }
    }
    return found ? bounds : GeoRect{};
}

}  // namespace

InvQuadIndex::InvQuadIndex(const Dataset& dataset,
                           const WebMercatorProjection& projection,
                           int height,
                           int delta)
    : InvQuadIndex(dataset, projection, height, delta, true) {}

InvQuadIndex::InvQuadIndex(const Dataset& dataset,
                           const WebMercatorProjection& projection,
                           int height,
                           int delta,
                           bool build_now)
    : dataset_(dataset), projection_(projection), height_(height), delta_(delta) {
    if (height < 1) {
        throw std::invalid_argument("La altura de InvQuad debe ser al menos 1");
    }
    if (delta < 0) {
        throw std::invalid_argument("delta no puede ser negativo");
    }
    if (build_now) {
        std::vector<int> ids;
        ids.reserve(dataset.trajectories.size());
        for (const auto& trajectory : dataset.trajectories) {
            ids.push_back(trajectory.id);
        }
        root_ = build(dataset_bounds(dataset), ids, height);
    }
}

bool InvQuadIndex::trajectory_intersects(int id, const GeoRect& region) const {
    const auto& trajectory = dataset_.trajectories[static_cast<std::size_t>(id)];
    return std::any_of(trajectory.locations.begin(), trajectory.locations.end(),
                       [&](const Location& location) { return region.contains(location); });
}

std::vector<InvQuadEntry> InvQuadIndex::build_entries(
    const GeoRect& bounds,
    const std::vector<int>& ids) const {
    Dataset local;
    std::vector<int> local_to_original;
    local.trajectories.reserve(ids.size());
    local_to_original.reserve(ids.size());

    for (int original_id : ids) {
        const auto& original = dataset_.trajectories[static_cast<std::size_t>(original_id)];
        Trajectory segment;
        segment.id = static_cast<int>(local.trajectories.size());
        std::unordered_set<Pixel, PixelHash> unique;
        for (const auto& location : original.locations) {
            if (bounds.contains(location)) {
                segment.locations.push_back(location);
                unique.insert(projection_.project(location));
            }
        }
        if (unique.empty()) {
            continue;
        }
        segment.pixels.assign(unique.begin(), unique.end());
        std::sort(segment.pixels.begin(), segment.pixels.end());
        segment.original_score = static_cast<int>(segment.pixels.size());
        local_to_original.push_back(original_id);
        local.trajectories.push_back(std::move(segment));
    }

    if (local.trajectories.empty()) {
        return {};
    }

    VqgsOptions options;
    options.delta = delta_;
    options.sample_count = local.trajectories.size();
    const VqgsResult result = VqgsEngine(local).select(options);

    std::vector<InvQuadEntry> entries;
    entries.reserve(result.trajectory_ids.size());
    for (std::size_t index = 0; index < result.trajectory_ids.size(); ++index) {
        const int local_id = result.trajectory_ids[index];
        entries.push_back({
            local_to_original[static_cast<std::size_t>(local_id)],
            result.cumulative_quality[index]
        });
        if (result.cumulative_quality[index] >= 1.0) {
            break;
        }
    }
    return entries;
}

std::unique_ptr<InvQuadIndex::Node> InvQuadIndex::build(
    const GeoRect& bounds,
    const std::vector<int>& candidate_ids,
    int remaining_height) const {
    auto node = std::make_unique<Node>();
    node->bounds = bounds;
    node->entries = build_entries(bounds, candidate_ids);
    if (remaining_height <= 1 || candidate_ids.empty()) {
        return node;
    }

    const double mid_lat = (bounds.min_latitude + bounds.max_latitude) / 2.0;
    const double mid_lon = (bounds.min_longitude + bounds.max_longitude) / 2.0;
    const GeoRect child_bounds[4] = {
        {bounds.min_latitude, mid_lat, bounds.min_longitude, mid_lon},
        {bounds.min_latitude, mid_lat, mid_lon, bounds.max_longitude},
        {mid_lat, bounds.max_latitude, bounds.min_longitude, mid_lon},
        {mid_lat, bounds.max_latitude, mid_lon, bounds.max_longitude}
    };

    node->children.reserve(4);
    for (const auto& child : child_bounds) {
        std::vector<int> child_ids;
        for (int id : candidate_ids) {
            if (trajectory_intersects(id, child)) {
                child_ids.push_back(id);
            }
        }
        node->children.push_back(build(child, child_ids, remaining_height - 1));
    }
    return node;
}

void InvQuadIndex::query_node(const Node& node,
                              const GeoRect& region,
                              double quality,
                              std::vector<int>& result,
                              std::vector<unsigned char>& seen) const {
    if (!node.bounds.intersects(region)) {
        return;
    }

    if (rect_contains(region, node.bounds) || node.children.empty()) {
        for (const auto& entry : node.entries) {
            if (seen[static_cast<std::size_t>(entry.trajectory_id)] == 0 &&
                trajectory_intersects(entry.trajectory_id, region)) {
                seen[static_cast<std::size_t>(entry.trajectory_id)] = 1;
                result.push_back(entry.trajectory_id);
            }
            if (entry.cumulative_quality >= quality) {
                break;
            }
        }
        return;
    }

    for (const auto& child : node.children) {
        query_node(*child, region, quality, result, seen);
    }
}

std::vector<int> InvQuadIndex::query(const GeoRect& region, double quality) const {
    if (quality < 0.0 || quality > 1.0) {
        throw std::invalid_argument("quality debe estar entre 0 y 1");
    }
    std::vector<int> result;
    std::vector<unsigned char> seen(dataset_.trajectories.size(), 0);
    query_node(*root_, region, quality, result, seen);
    return result;
}

void InvQuadIndex::save_node(const Node& node,
                             const std::string& path,
                             std::ostream& output) const {
    output << path << ','
           << std::setprecision(15)
           << node.bounds.min_latitude << ',' << node.bounds.max_latitude << ','
           << node.bounds.min_longitude << ',' << node.bounds.max_longitude << ','
           << node.entries.size();
    for (const auto& entry : node.entries) {
        output << ',' << entry.trajectory_id << ':' << entry.cumulative_quality;
    }
    output << '\n';
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        save_node(*node.children[index], path + std::to_string(index), output);
    }
}

void InvQuadIndex::save(const std::filesystem::path& path) const {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("No se pudo guardar InvQuad: " + path.string());
    }
    output << "# InvQuad C++ height=" << height_ << " delta=" << delta_ << '\n';
    save_node(*root_, "r", output);
}

std::unique_ptr<InvQuadIndex> InvQuadIndex::load(
    const Dataset& dataset,
    const WebMercatorProjection& projection,
    const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("No se pudo leer InvQuad: " + path.string());
    }
    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("InvQuad vacio: " + path.string());
    }
    int height = 0;
    int delta = 0;
    if (std::sscanf(line.c_str(), "# InvQuad C++ height=%d delta=%d",
                    &height, &delta) != 2) {
        throw std::runtime_error("Cabecera InvQuad invalida");
    }
    auto index = std::unique_ptr<InvQuadIndex>(
        new InvQuadIndex(dataset, projection, height, delta, false));
    std::unordered_map<std::string, Node*> nodes;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::stringstream row(line);
        std::string field;
        auto next = [&]() -> std::string {
            if (!std::getline(row, field, ',')) {
                throw std::runtime_error("Fila InvQuad incompleta");
            }
            return field;
        };
        const std::string node_path = next();
        if (node_path.empty() || node_path[0] != 'r') {
            throw std::runtime_error("Ruta de nodo InvQuad invalida");
        }
        auto node = std::make_unique<Node>();
        node->bounds = {std::stod(next()), std::stod(next()),
                        std::stod(next()), std::stod(next())};
        const auto entry_count = std::stoull(next());
        for (std::size_t i = 0; i < entry_count; ++i) {
            const std::string entry = next();
            const auto colon = entry.find(':');
            if (colon == std::string::npos) {
                throw std::runtime_error("Entrada InvQuad invalida");
            }
            const int id = std::stoi(entry.substr(0, colon));
            if (id < 0 || static_cast<std::size_t>(id) >=
                              dataset.trajectories.size()) {
                throw std::runtime_error("ID InvQuad fuera del dataset");
            }
            node->entries.push_back({id, std::stod(entry.substr(colon + 1))});
        }
        if (node_path == "r") {
            if (index->root_) throw std::runtime_error("Raiz InvQuad duplicada");
            index->root_ = std::move(node);
            nodes[node_path] = index->root_.get();
        } else {
            const auto parent_path = node_path.substr(0, node_path.size() - 1);
            const auto parent = nodes.find(parent_path);
            if (parent == nodes.end()) {
                throw std::runtime_error("Nodo padre InvQuad ausente");
            }
            const char digit = node_path.back();
            if (digit < '0' || digit > '3' ||
                parent->second->children.size() !=
                    static_cast<std::size_t>(digit - '0')) {
                throw std::runtime_error("Orden de hijos InvQuad invalido");
            }
            parent->second->children.push_back(std::move(node));
            nodes[node_path] = parent->second->children.back().get();
        }
    }
    if (!index->root_) throw std::runtime_error("InvQuad sin raiz");
    return index;
}

const GeoRect& InvQuadIndex::bounds() const noexcept {
    return root_->bounds;
}

}  // namespace cheetah

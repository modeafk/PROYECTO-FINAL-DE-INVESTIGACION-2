#include "cheetah/temporal_invquad.hpp"

#include "cheetah/invquad.hpp"
#include "cheetah/io.hpp"
#include "cheetah/vqgs.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace cheetah {
namespace {

std::string window_stem(std::size_t index) {
    return "window_" + std::to_string(index);
}

void save_window(const Dataset& dataset, const std::filesystem::path& path) {
    std::ofstream output(path);
    if (!output) throw std::runtime_error("No se pudo guardar " + path.string());
    output << "trajectory_id,longitude,latitude,timestamp\n"
           << std::setprecision(17);
    for (const auto& trajectory : dataset.trajectories) {
        for (const auto& location : trajectory.locations) {
            output << trajectory.source_id << ',' << location.longitude << ','
                   << location.latitude << ',' << location.timestamp << '\n';
        }
    }
}

void validate_region(const GeoRect& region) {
    if (region.min_latitude > region.max_latitude ||
        region.min_longitude > region.max_longitude) {
        throw std::invalid_argument("Region espacial invalida");
    }
}

}  // namespace

TemporalIndexBuildResult TemporalInvQuadIndex::build(
    const Dataset& complete,
    const std::filesystem::path& output,
    const TemporalIndexOptions& options) {
    if (options.height < 1 || options.delta < 0 ||
        options.minimum_points < 1) {
        throw std::invalid_argument("Altura, delta o min-points invalidos");
    }
    const auto bounds = dataset_time_bounds(complete);
    if (!bounds) throw std::invalid_argument("Dataset sin timestamps");
    const auto windows = generate_time_windows(bounds->first, bounds->second,
                                               options.window_size,
                                               options.window_step);
    std::filesystem::create_directories(output);
    std::ofstream manifest(output / "manifest.csv");
    if (!manifest) throw std::runtime_error("No se pudo guardar manifest.csv");
    manifest << "# temporal_invquad_v1\n"
             << "zoom,center_latitude,center_longitude,width,height,"
                "tree_height,delta,min_points\n"
             << std::setprecision(17)
             << options.projection.zoom << ','
             << options.projection.center_latitude << ','
             << options.projection.center_longitude << ','
             << options.projection.viewport_width << ','
             << options.projection.viewport_height << ','
             << options.height << ',' << options.delta << ','
             << options.minimum_points << '\n'
             << "window,start,end,segments\n";

    TemporalIndexBuildResult report;
    report.windows = windows.size();
    const WebMercatorProjection projection(options.projection);
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const Dataset current = filter_by_time_window(
            complete, windows[i], {options.minimum_points});
        manifest << i << ',' << windows[i].start << ',' << windows[i].end
                 << ',' << current.trajectories.size() << '\n';
        report.stored_segments += current.trajectories.size();
        if (current.trajectories.empty()) continue;
        ++report.nonempty_windows;
        const auto stem = window_stem(i);
        save_window(current, output / (stem + ".csv"));
        InvQuadIndex index(current, projection, options.height, options.delta);
        index.save(output / (stem + ".invquad.csv"));
    }
    return report;
}

TemporalQueryResult TemporalInvQuadIndex::query(
    const std::filesystem::path& index_directory,
    std::size_t window_index,
    const GeoRect& region,
    double quality_target) {
    validate_region(region);
    if (quality_target <= 0.0 || quality_target > 1.0) {
        throw std::invalid_argument("quality-target debe estar en (0,1]");
    }
    std::ifstream manifest(index_directory / "manifest.csv");
    std::string line;
    if (!manifest || !std::getline(manifest, line) ||
        line != "# temporal_invquad_v1") {
        throw std::runtime_error("Manifest temporal ausente o incompatible");
    }
    std::getline(manifest, line); // Header for projection settings.
    ProjectionOptions projection_options;
    int tree_height = 0;
    int delta = 0;
    std::size_t minimum_points = 0;
    if (!(manifest >> projection_options.zoom)) {
        throw std::runtime_error("Manifest temporal incompleto");
    }
    auto comma = [&]() {
        char ch = 0;
        if (!(manifest >> ch) || ch != ',')
            throw std::runtime_error("Manifest temporal invalido");
    };
    comma(); manifest >> projection_options.center_latitude;
    comma(); manifest >> projection_options.center_longitude;
    comma(); manifest >> projection_options.viewport_width;
    comma(); manifest >> projection_options.viewport_height;
    comma(); manifest >> tree_height;
    comma(); manifest >> delta;
    comma(); manifest >> minimum_points;
    if (!manifest || tree_height < 1 || delta < 0) {
        throw std::runtime_error("Parametros del manifest invalidos");
    }
    std::getline(manifest, line); // Finish settings row.
    std::getline(manifest, line); // Window header.

    TemporalQueryResult answer;
    bool found = false;
    while (std::getline(manifest, line)) {
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream row(line);
        std::size_t id = 0;
        TimeWindow window;
        std::size_t count = 0;
        if (!(row >> id >> window.start >> window.end >> count)) {
            throw std::runtime_error("Fila de ventana invalida");
        }
        if (id == window_index) {
            answer.window = window;
            answer.available_trajectories = count;
            found = true;
            break;
        }
    }
    if (!found) throw std::out_of_range("Ventana inexistente en el indice");
    if (answer.available_trajectories == 0) {
        answer.quality = 1.0;
        answer.target_met = true;
        return answer;
    }

    const auto stem = window_stem(window_index);
    Dataset window = load_temporal_dataset(index_directory / (stem + ".csv"));
    if (window.trajectories.size() != answer.available_trajectories) {
        throw std::runtime_error("El dataset de ventana no coincide con el manifest");
    }
    const WebMercatorProjection projection(projection_options);
    const auto index = InvQuadIndex::load(
        window, projection, index_directory / (stem + ".invquad.csv"));
    std::vector<int> selected = index->query(region, quality_target);
    answer.indexed_selection_count = selected.size();

    Dataset clipped;
    std::vector<int> clipped_to_window;
    std::unordered_map<int, int> window_to_clipped;
    for (const auto& trajectory : window.trajectories) {
        Trajectory segment;
        segment.id = static_cast<int>(clipped.trajectories.size());
        segment.source_id = trajectory.source_id;
        for (const auto& location : trajectory.locations) {
            if (region.contains(location)) segment.locations.push_back(location);
        }
        if (segment.locations.empty()) continue;
        clipped_to_window.push_back(trajectory.id);
        window_to_clipped.emplace(trajectory.id, segment.id);
        clipped.trajectories.push_back(std::move(segment));
    }
    if (clipped.trajectories.empty()) {
        answer.quality = 1.0;
        answer.target_met = true;
        return answer;
    }
    rasterize_dataset(clipped, projection);
    auto measured_quality = [&]() {
        std::vector<int> local_ids;
        local_ids.reserve(selected.size());
        for (int window_id : selected) {
            const auto found_id = window_to_clipped.find(window_id);
            if (found_id != window_to_clipped.end())
                local_ids.push_back(found_id->second);
        }
        return quality_at_projection(clipped, local_ids, projection, delta);
    };
    answer.quality = measured_quality();

    // A node touching the query boundary has a quality value measured for
    // its whole extent. Complete the sample when the actual clipped query
    // falls short of the requested threshold.
    if (answer.quality + 1e-12 < quality_target) {
        VqgsOptions greedy_options;
        greedy_options.delta = delta;
        greedy_options.quality_target = quality_target;
        const auto greedy = VqgsEngine(clipped).select(greedy_options);
        std::unordered_set<int> seen(selected.begin(), selected.end());
        for (int local_id : greedy.trajectory_ids) {
            const int window_id = clipped_to_window[
                static_cast<std::size_t>(local_id)];
            if (seen.insert(window_id).second) {
                selected.push_back(window_id);
                ++answer.supplemented_count;
            }
            answer.quality = measured_quality();
            if (answer.quality + 1e-12 >= quality_target) break;
        }
    }
    answer.target_met = answer.quality + 1e-12 >= quality_target;
    for (int id : selected) {
        answer.source_ids.push_back(
            window.trajectories[static_cast<std::size_t>(id)].source_id);
    }
    return answer;
}

}  // namespace cheetah

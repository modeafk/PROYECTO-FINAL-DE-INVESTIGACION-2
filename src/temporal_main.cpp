#include "cheetah/io.hpp"
#include "cheetah/projection.hpp"
#include "cheetah/temporal.hpp"
#include "cheetah/vqgs.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

// Alpha por defecto. Puede cambiarse desde la terminal con --rate.
constexpr double kDefaultAlpha = 0.005;

struct Options {
    std::filesystem::path dataset{"data/temporal_example.csv"};
    std::filesystem::path output{"output/temporal_baseline"};
    std::string format{"generic"};
    std::size_t limit{0};
    std::optional<cheetah::Timestamp> start_time;
    std::optional<cheetah::Timestamp> end_time;
    cheetah::Timestamp point_interval{15};
    cheetah::Timestamp window_size{3600};
    cheetah::Timestamp window_step{3600};
    std::size_t minimum_points{2};
    // sample_count == 0 activa el calculo k_j = ceil(alpha * |T_j|).
    std::size_t sample_count{0};
    double sampling_rate{kDefaultAlpha};
    double quality_target{0.0};
    int delta{4};
    int zoom{15};
};

void print_help() {
    std::cout
        << "Baseline temporal de CheetahTraj\n\n"
        << "Uso: cheetahtraj-temporal [opciones]\n\n"
        << "  --dataset ARCHIVO       CSV: trajectory_id,longitude,latitude,timestamp\n"
        << "  --format TIPO           generic o porto\n"
        << "  --limit N               Maximo de trayectorias aceptadas\n"
        << "  --from UNIX             Primer segundo incluido\n"
        << "  --to UNIX               Primer segundo excluido\n"
        << "  --point-interval SEG    Intervalo entre puntos Porto (15)\n"
        << "  --output DIR            Directorio de resultados\n"
        << "  --window-size SEG       Duracion de ventana en segundos\n"
        << "  --window-step SEG       Desplazamiento entre ventanas\n"
        << "  --min-points N          Puntos minimos por segmento\n"
        << "  --count N               Cantidad exacta por ventana (reemplaza alpha)\n"
        << "  --rate ALPHA            Tasa: k_j = ceil(ALPHA * |T_j|)\n"
        << "  --quality-target N      Calidad objetivo; 0 usa presupuesto fijo\n"
        << "  --delta N               Tolerancia perceptual\n"
        << "  --zoom N                Zoom de rasterizacion\n";
}

Options parse_arguments(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&]() -> std::string {
            if (index + 1 >= argc) {
                throw std::invalid_argument("Falta valor para " + argument);
            }
            return argv[++index];
        };
        if (argument == "--help" || argument == "-h") {
            print_help();
            std::exit(0);
        } else if (argument == "--dataset") {
            options.dataset = value();
        } else if (argument == "--format") {
            options.format = value();
        } else if (argument == "--limit") {
            options.limit = std::stoull(value());
        } else if (argument == "--from") {
            options.start_time = std::stoll(value());
        } else if (argument == "--to") {
            options.end_time = std::stoll(value());
        } else if (argument == "--point-interval") {
            options.point_interval = std::stoll(value());
        } else if (argument == "--output") {
            options.output = value();
        } else if (argument == "--window-size") {
            options.window_size = std::stoll(value());
        } else if (argument == "--window-step") {
            options.window_step = std::stoll(value());
        } else if (argument == "--min-points") {
            options.minimum_points = std::stoull(value());
        } else if (argument == "--count") {
            options.sample_count = std::stoull(value());
            options.quality_target = 0.0;
        } else if (argument == "--rate") {
            options.sampling_rate = std::stod(value());
            options.sample_count = 0;
            options.quality_target = 0.0;
        } else if (argument == "--quality-target") {
            options.quality_target = std::stod(value());
        } else if (argument == "--delta") {
            options.delta = std::stoi(value());
        } else if (argument == "--zoom") {
            options.zoom = std::stoi(value());
        } else {
            throw std::invalid_argument("Opcion desconocida: " + argument);
        }
    }
    return options;
}

std::filesystem::path resolve_path(const std::filesystem::path& requested,
                                   const std::filesystem::path& executable) {
    if (std::filesystem::exists(requested)) {
        return requested;
    }
    const auto executable_dir =
        std::filesystem::absolute(executable).parent_path();
    const std::filesystem::path candidates[] = {
        executable_dir / requested,
        executable_dir.parent_path() / requested,
        executable_dir.parent_path().parent_path() / requested
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    throw std::runtime_error("No se encontro el dataset temporal: " +
                             requested.string());
}

cheetah::ProjectionOptions projection_for(const cheetah::Dataset& dataset,
                                          int zoom) {
    double min_lat = std::numeric_limits<double>::max();
    double max_lat = std::numeric_limits<double>::lowest();
    double min_lon = std::numeric_limits<double>::max();
    double max_lon = std::numeric_limits<double>::lowest();
    for (const auto& trajectory : dataset.trajectories) {
        for (const auto& location : trajectory.locations) {
            min_lat = std::min(min_lat, location.latitude);
            max_lat = std::max(max_lat, location.latitude);
            min_lon = std::min(min_lon, location.longitude);
            max_lon = std::max(max_lon, location.longitude);
        }
    }
    cheetah::ProjectionOptions options;
    options.zoom = zoom;
    options.center_latitude = (min_lat + max_lat) / 2.0;
    options.center_longitude = (min_lon + max_lon) / 2.0;
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options options = parse_arguments(argc, argv);
        options.dataset = resolve_path(options.dataset, argv[0]);
        cheetah::Dataset complete;
        if (options.format == "generic") {
            complete = cheetah::load_temporal_dataset(
                options.dataset, {options.limit, false});
        } else if (options.format == "porto") {
            if (options.limit == 0 &&
                (!options.start_time || !options.end_time)) {
                throw std::invalid_argument(
                    "Para train.csv indique --limit o el rango --from/--to; "
                    "cargar 1.94 GB completos no es seguro");
            }
            cheetah::PortoLoadOptions porto_options;
            porto_options.limit = options.limit;
            porto_options.minimum_points = options.minimum_points;
            porto_options.point_interval_seconds = options.point_interval;
            porto_options.start_time = options.start_time;
            porto_options.end_time = options.end_time;
            complete = cheetah::load_porto_train_dataset(
                options.dataset, porto_options);
        } else {
            throw std::invalid_argument("Formato desconocido: " + options.format);
        }
        if (complete.trajectories.empty()) {
            throw std::runtime_error(
                "Ninguna trayectoria cumple los filtros temporales");
        }
        const auto bounds = cheetah::dataset_time_bounds(complete);
        if (!bounds) {
            throw std::runtime_error("El dataset no contiene timestamps");
        }
        const auto windows = cheetah::generate_time_windows(
            bounds->first, bounds->second,
            options.window_size, options.window_step);
        const cheetah::ProjectionOptions projection_options =
            projection_for(complete, options.zoom);

        std::filesystem::create_directories(options.output);
        std::ofstream summary(options.output / "window_summary.csv");
        std::ofstream selections(options.output / "window_selections.csv");
        if (!summary || !selections) {
            throw std::runtime_error("No se pudieron crear los resultados temporales");
        }
        summary << "window,start,end,trajectories,selected,alpha_configured,"
                   "alpha_actual,quality,quality_target,target_met,time_ms\n";
        selections << "window,source_id,local_id,rank,marginal_score,cumulative_quality\n";

        std::cout << "Dataset temporal: " << options.dataset.string() << '\n'
                  << "Trayectorias fuente: " << complete.trajectories.size() << '\n'
                  << "Ventanas: " << windows.size() << '\n';
        if (options.quality_target > 0.0) {
            std::cout << "Modo: calidad objetivo Q >= "
                      << options.quality_target << "\n\n";
        } else if (options.sample_count > 0) {
            std::cout << "Modo: cantidad fija k = "
                      << options.sample_count << "\n\n";
        } else {
            std::cout << "Modo: tasa fija alpha = "
                      << options.sampling_rate << "\n\n";
        }

        for (std::size_t window_index = 0;
             window_index < windows.size(); ++window_index) {
            const auto start = std::chrono::steady_clock::now();
            cheetah::Dataset current = cheetah::filter_by_time_window(
                complete, windows[window_index], {options.minimum_points});

            std::vector<int> selected_ids;
            std::vector<int> marginal_scores;
            std::vector<double> cumulative_quality;
            double quality = std::numeric_limits<double>::quiet_NaN();
            if (!current.trajectories.empty()) {
                cheetah::rasterize_dataset(
                    current, cheetah::WebMercatorProjection(projection_options));
                const cheetah::VqgsEngine engine(current);
                cheetah::VqgsOptions vqgs_options;
                vqgs_options.delta = options.delta;
                vqgs_options.sample_count = options.sample_count;
                vqgs_options.sampling_rate = options.sampling_rate;
                vqgs_options.quality_target = options.quality_target;
                const auto result = engine.select(vqgs_options);
                quality = engine.quality(result);
                selected_ids = result.trajectory_ids;
                marginal_scores = result.marginal_scores;
                cumulative_quality = result.cumulative_quality;
            }

            for (std::size_t rank = 0; rank < selected_ids.size(); ++rank) {
                const int local_id = selected_ids[rank];
                const auto& trajectory =
                    current.trajectories[static_cast<std::size_t>(local_id)];
                selections << window_index << ',' << trajectory.source_id << ','
                           << local_id << ',' << rank << ','
                           << marginal_scores[rank] << ','
                           << std::setprecision(10) << cumulative_quality[rank]
                           << '\n';
            }

            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            const double actual_rate = current.trajectories.empty()
                ? 0.0
                : static_cast<double>(selected_ids.size()) /
                      static_cast<double>(current.trajectories.size());
            const double configured_alpha =
                options.quality_target <= 0.0 && options.sample_count == 0
                    ? options.sampling_rate
                    : 0.0;
            const bool target_met = options.quality_target <= 0.0 ||
                                    (!current.trajectories.empty() &&
                                     quality >= options.quality_target);
            summary << window_index << ',' << windows[window_index].start << ','
                    << windows[window_index].end << ','
                    << current.trajectories.size() << ',' << selected_ids.size()
                    << ',' << std::setprecision(10) << configured_alpha
                    << ',' << actual_rate
                    << ',' << quality << ',' << options.quality_target << ','
                    << (target_met ? "true" : "false") << ',' << elapsed << '\n';
            std::cout << "W" << window_index << " ["
                      << windows[window_index].start << ", "
                      << windows[window_index].end << "): "
                      << current.trajectories.size() << " trayectorias, "
                      << selected_ids.size() << " seleccionadas, alpha="
                      << std::fixed << std::setprecision(6) << actual_rate
                      << ", Q=";
            if (current.trajectories.empty()) {
                std::cout << "N/A\n";
            } else {
                std::cout << std::fixed << std::setprecision(6)
                          << quality << '\n';
            }
        }

        std::cout << "\nResultados: "
                  << std::filesystem::absolute(options.output).string() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}

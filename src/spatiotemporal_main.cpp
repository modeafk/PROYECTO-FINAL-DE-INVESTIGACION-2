#include "cheetah/io.hpp"
#include "cheetah/temporal_invquad.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string command{"interactive"};
    std::filesystem::path dataset{"data/temporal_example.csv"};
    std::filesystem::path index_directory{"output/temporal_invquad"};
    bool explicit_index_directory{false};
    std::string format{"generic"};
    std::size_t limit{0};
    std::optional<cheetah::Timestamp> from;
    std::optional<cheetah::Timestamp> to;
    std::optional<cheetah::Timestamp> at;
    std::optional<std::size_t> window_index;
    cheetah::GeoRect region;
    bool has_region{false};
    double quality_target{0.9};
    cheetah::TemporalIndexOptions index;
};

void help() {
    std::cout
        << "Indice espacial-temporal de CheetahTraj\n\n"
        << "Menu interactivo (muestra ventanas y horarios UTC):\n"
        << "  cheetahtraj-spatiotemporal\n"
        << "  cheetahtraj-spatiotemporal interactive --index-dir output/indice\n\n"
        << "Construccion offline:\n"
        << "  cheetahtraj-spatiotemporal build --format porto --dataset train.csv"
           " --limit 1000 --index-dir output/indice\n\n"
        << "Consulta online:\n"
        << "  cheetahtraj-spatiotemporal query --index-dir output/indice"
           " --window 0 --bbox 41.1 41.2 -8.7 -8.5 --quality-target 0.9\n\n"
        << "Opciones build: --format generic|porto, --dataset RUTA, --limit N,"
           " --from UNIX, --to UNIX, --window-size SEG, --window-step SEG,"
           " --min-points N, --height N, --delta N, --zoom N\n"
        << "Opciones query: --window N o --at UNIX, --bbox minLat maxLat"
           " minLon maxLon, --quality-target Q\n";
}

Options parse(int argc, char** argv) {
    Options options;
    int first_option = 1;
    if (argc >= 2 && argv[1][0] != '-') {
        options.command = argv[1];
        first_option = 2;
    }
    if (options.command == "--help") { help(); std::exit(0); }
    for (int i = first_option; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument("Falta valor para " + arg);
            return argv[++i];
        };
        if (arg == "--help") { help(); std::exit(0); }
        else if (arg == "--dataset") options.dataset = next();
        else if (arg == "--index-dir") {
            options.index_directory = next();
            options.explicit_index_directory = true;
        }
        else if (arg == "--format") options.format = next();
        else if (arg == "--limit") options.limit = std::stoull(next());
        else if (arg == "--from") options.from = std::stoll(next());
        else if (arg == "--to") options.to = std::stoll(next());
        else if (arg == "--window-size") options.index.window_size = std::stoll(next());
        else if (arg == "--window-step") options.index.window_step = std::stoll(next());
        else if (arg == "--min-points") options.index.minimum_points = std::stoull(next());
        else if (arg == "--height") options.index.height = std::stoi(next());
        else if (arg == "--delta") options.index.delta = std::stoi(next());
        else if (arg == "--zoom") options.index.projection.zoom = std::stoi(next());
        else if (arg == "--window") options.window_index = std::stoull(next());
        else if (arg == "--at") options.at = std::stoll(next());
        else if (arg == "--quality-target") options.quality_target = std::stod(next());
        else if (arg == "--bbox") {
            options.region.min_latitude = std::stod(next());
            options.region.max_latitude = std::stod(next());
            options.region.min_longitude = std::stod(next());
            options.region.max_longitude = std::stod(next());
            options.has_region = true;
        } else {
            throw std::invalid_argument("Opcion desconocida: " + arg);
        }
    }
    return options;
}

cheetah::ProjectionOptions projection_for(const cheetah::Dataset& dataset,
                                          int zoom) {
    double min_lat = std::numeric_limits<double>::max();
    double max_lat = std::numeric_limits<double>::lowest();
    double min_lon = std::numeric_limits<double>::max();
    double max_lon = std::numeric_limits<double>::lowest();
    for (const auto& trajectory : dataset.trajectories) {
        for (const auto& point : trajectory.locations) {
            min_lat = std::min(min_lat, point.latitude);
            max_lat = std::max(max_lat, point.latitude);
            min_lon = std::min(min_lon, point.longitude);
            max_lon = std::max(max_lon, point.longitude);
        }
    }
    cheetah::ProjectionOptions options;
    options.zoom = zoom;
    options.center_latitude = (min_lat + max_lat) / 2.0;
    options.center_longitude = (min_lon + max_lon) / 2.0;
    return options;
}

std::size_t window_for_timestamp(const std::filesystem::path& index_directory,
                                 cheetah::Timestamp timestamp) {
    std::ifstream input(index_directory / "manifest.csv");
    std::string line;
    if (!input) throw std::runtime_error("No se encontro manifest.csv");
    for (int i = 0; i < 4; ++i) std::getline(input, line);
    while (std::getline(input, line)) {
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream row(line);
        std::size_t id, count;
        cheetah::Timestamp start, end;
        if (row >> id >> start >> end >> count &&
            timestamp >= start && timestamp < end) return id;
    }
    throw std::out_of_range("El instante no pertenece a ninguna ventana indexada");
}

struct WindowInfo {
    std::size_t id{};
    cheetah::Timestamp start{};
    cheetah::Timestamp end{};
    std::size_t segments{};
};

std::vector<WindowInfo> read_windows(const std::filesystem::path& directory) {
    std::ifstream input(directory / "manifest.csv");
    std::string line;
    if (!input || !std::getline(input, line) ||
        line != "# temporal_invquad_v1") {
        throw std::runtime_error(
            "No se encontro un indice temporal valido en " + directory.string());
    }
    for (int i = 0; i < 3; ++i) {
        if (!std::getline(input, line))
            throw std::runtime_error("Manifest temporal incompleto");
    }
    std::vector<WindowInfo> windows;
    while (std::getline(input, line)) {
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream row(line);
        WindowInfo info;
        if (!(row >> info.id >> info.start >> info.end >> info.segments)) {
            throw std::runtime_error("Fila de ventana invalida en manifest.csv");
        }
        windows.push_back(info);
    }
    return windows;
}

std::string format_utc(cheetah::Timestamp timestamp) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp);
    std::tm calendar{};
#ifdef _WIN32
    if (gmtime_s(&calendar, &seconds) != 0)
        throw std::runtime_error("Timestamp UTC invalido");
#else
    if (gmtime_r(&seconds, &calendar) == nullptr)
        throw std::runtime_error("Timestamp UTC invalido");
#endif
    std::ostringstream output;
    output << std::put_time(&calendar, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

cheetah::GeoRect full_window_region(const std::filesystem::path& directory,
                                    std::size_t window_index) {
    const auto path = directory /
        ("window_" + std::to_string(window_index) + ".csv");
    const auto dataset = cheetah::load_temporal_dataset(path);
    cheetah::GeoRect region{
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest()
    };
    for (const auto& trajectory : dataset.trajectories) {
        for (const auto& point : trajectory.locations) {
            region.min_latitude = std::min(region.min_latitude, point.latitude);
            region.max_latitude = std::max(region.max_latitude, point.latitude);
            region.min_longitude = std::min(region.min_longitude, point.longitude);
            region.max_longitude = std::max(region.max_longitude, point.longitude);
        }
    }
    if (dataset.trajectories.empty())
        throw std::runtime_error("La ventana no tiene puntos consultables");
    return region;
}

void print_query_result(const cheetah::TemporalQueryResult& result,
                        std::size_t window,
                        long long milliseconds,
                        bool brief) {
    std::cout << "Ventana W" << window << " [" << format_utc(result.window.start)
              << ", " << format_utc(result.window.end) << ") UTC\n"
              << "Segmentos de la ventana: " << result.available_trajectories
              << '\n' << "Trayectorias recuperadas del indice: "
              << result.indexed_selection_count << '\n'
              << "Trayectorias agregadas para alcanzar Q: "
              << result.supplemented_count << '\n'
              << "Q de la consulta: " << std::fixed << std::setprecision(6)
              << result.quality << '\n'
              << "Objetivo alcanzado: "
              << (result.target_met ? "si" : "no") << '\n'
              << "Tiempo online (ms): " << milliseconds << '\n'
              << "Trayectorias seleccionadas: " << result.source_ids.size()
              << '\n' << "TRIP_ID:";
    const std::size_t visible = brief
        ? std::min<std::size_t>(10, result.source_ids.size())
        : result.source_ids.size();
    for (std::size_t i = 0; i < visible; ++i)
        std::cout << (brief ? "\n  " : " ") << result.source_ids[i];
    if (visible < result.source_ids.size())
        std::cout << "\n  ... (primeros 10; use query para la lista completa)";
    std::cout << '\n';
}

void interactive(std::filesystem::path directory) {
    const auto windows = read_windows(directory);
    std::cout << "Indice: " << directory << '\n';
    while (true) {
        std::cout << "\nVentanas disponibles (horarios UTC):\n";
        for (const auto& window : windows) {
            std::cout << "  W" << window.id << "  "
                      << format_utc(window.start) << " a "
                      << format_utc(window.end) << "  ("
                      << window.segments << " segmentos)\n";
        }
        std::cout << "\nElige una ventana (numero, o 'q' para salir): ";
        std::string line;
        if (!std::getline(std::cin, line) || line == "q" || line == "Q")
            return;
        try {
            std::size_t consumed = 0;
            const auto selected = std::stoull(line, &consumed);
            if (consumed != line.size())
                throw std::invalid_argument("Numero de ventana invalido");
            const auto found = std::find_if(
                windows.begin(), windows.end(),
                [&](const WindowInfo& item) { return item.id == selected; });
            if (found == windows.end())
                throw std::out_of_range("Esa ventana no existe");
            if (found->segments == 0) {
                std::cout << "Ventana sin trayectorias. Elige otra.\n";
                continue;
            }
            std::cout << "Calidad objetivo Q (Enter = 0.90): ";
            if (!std::getline(std::cin, line)) return;
            const double quality = line.empty() ? 0.9 : std::stod(line);
            if (quality <= 0.0 || quality > 1.0)
                throw std::invalid_argument("Q debe estar entre 0 y 1");
            std::cout << "Region minLat maxLat minLon maxLon "
                         "(Enter = region completa): ";
            if (!std::getline(std::cin, line)) return;
            cheetah::GeoRect region;
            if (line.empty()) {
                region = full_window_region(directory, selected);
            } else {
                std::istringstream values(line);
                std::string extra;
                if (!(values >> region.min_latitude >> region.max_latitude >>
                      region.min_longitude >> region.max_longitude) ||
                    (values >> extra)) {
                    throw std::invalid_argument(
                        "Escribe cuatro coordenadas separadas por espacios");
                }
            }
            const auto start = std::chrono::steady_clock::now();
            const auto result = cheetah::TemporalInvQuadIndex::query(
                directory, selected, region, quality);
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
            std::cout << '\n';
            print_query_result(result, selected, elapsed.count(), true);
        } catch (const std::exception& error) {
            std::cout << "Consulta no realizada: " << error.what() << '\n';
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options options = parse(argc, argv);
        if (options.command == "interactive") {
            if (!options.explicit_index_directory &&
                !std::filesystem::exists(
                    options.index_directory / "manifest.csv") &&
                std::filesystem::exists(
                    "output/porto_st_index_1k/manifest.csv")) {
                options.index_directory = "output/porto_st_index_1k";
            }
            interactive(options.index_directory);
        } else if (options.command == "build") {
            cheetah::Dataset dataset;
            if (options.format == "porto") {
                if (options.limit == 0 && (!options.from || !options.to)) {
                    throw std::invalid_argument(
                        "Para Porto indique --limit o --from y --to");
                }
                cheetah::PortoLoadOptions loader;
                loader.limit = options.limit;
                loader.minimum_points = options.index.minimum_points;
                loader.start_time = options.from;
                loader.end_time = options.to;
                dataset = cheetah::load_porto_train_dataset(options.dataset, loader);
            } else if (options.format == "generic") {
                dataset = cheetah::load_temporal_dataset(options.dataset,
                                                         {options.limit, false});
            } else {
                throw std::invalid_argument("Formato desconocido");
            }
            if (dataset.trajectories.empty())
                throw std::runtime_error("No se cargaron trayectorias");
            options.index.projection = projection_for(
                dataset, options.index.projection.zoom);
            const auto start = std::chrono::steady_clock::now();
            const auto report = cheetah::TemporalInvQuadIndex::build(
                dataset, options.index_directory, options.index);
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            std::cout << "Indice preparado: " << options.index_directory << '\n'
                      << "Ventanas: " << report.windows
                      << ", no vacias: " << report.nonempty_windows
                      << ", segmentos: " << report.stored_segments << '\n'
                      << "Tiempo offline (ms): " << elapsed.count() << '\n';
        } else if (options.command == "query") {
            if (!options.has_region) throw std::invalid_argument("Falta --bbox");
            if (options.window_index && options.at)
                throw std::invalid_argument("Use --window o --at, no ambos");
            if (!options.window_index && !options.at)
                throw std::invalid_argument("Indique --window o --at");
            const std::size_t window = options.window_index.value_or(
                options.at ? window_for_timestamp(options.index_directory,
                                                  *options.at) : 0);
            const auto start = std::chrono::steady_clock::now();
            const auto result = cheetah::TemporalInvQuadIndex::query(
                options.index_directory, window, options.region,
                options.quality_target);
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            print_query_result(result, window, elapsed.count(), false);
        } else {
            help();
            throw std::invalid_argument("Comando desconocido");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}

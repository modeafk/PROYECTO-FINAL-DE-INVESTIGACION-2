#include "cheetah/io.hpp"
#include "cheetah/invquad.hpp"
#include "cheetah/projection.hpp"
#include "cheetah/quadtree.hpp"
#include "cheetah/vqgs.hpp"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

// Configuracion predeterminada. Al ejecutar el programa sin argumentos se
// utilizan estos valores; los parametros de consola solo sirven para realizar
// experimentos distintos sin recompilar.
constexpr const char* kDefaultDataset = "data/porto_5k/porto_5k.txt";
constexpr const char* kDefaultOutput = "output/porto_default";
constexpr double kDefaultRate = 0.005;
constexpr int kDefaultDelta = 64;
constexpr int kDefaultZoom = 20;
constexpr int kDefaultQualityMinZoom = 11;
constexpr int kDefaultQualityMaxZoom = 20;

struct CliOptions {
    std::filesystem::path dataset{kDefaultDataset};
    std::filesystem::path output{kDefaultOutput};
    double rate{kDefaultRate};
    std::size_t count{0};
    int delta{kDefaultDelta};
    std::size_t limit{0};
    int zoom{kDefaultZoom};
    double center_latitude{42.0273551940919};
    double center_longitude{-9.179738998413087};
    int width{1200};
    int height{800};
    int index_height{0};
    int invquad_height{0};
    double index_quality{0.9};
    int quality_min_zoom{kDefaultQualityMinZoom};
    int quality_max_zoom{kDefaultQualityMaxZoom};
    bool java_loader_compatible{false};
};

void print_help() {
    std::cout
        << "CheetahTraj C++ core port\n\n"
        << "Uso:\n"
        << "  cheetahtraj-cli                 Ejecutar configuracion predeterminada\n"
        << "  cheetahtraj-cli [opciones]      Sobrescribir valores predeterminados\n\n"
        << "Opciones:\n"
        << "  --output DIR             Directorio de resultados (output)\n"
        << "  --rate N                 Tasa de muestreo (0.005)\n"
        << "  --count N                Cantidad exacta; reemplaza --rate\n"
        << "  --delta N                Tolerancia VQGS+ en pixeles (64)\n"
        << "  --limit N                Limitar trayectorias; 0 = todas\n"
        << "  --zoom N                 Zoom Web Mercator (20)\n"
        << "  --center-lat N           Latitud central\n"
        << "  --center-lon N           Longitud central\n"
        << "  --width N                Ancho del lienzo (1200)\n"
        << "  --height N               Alto del lienzo (800)\n"
        << "  --index-height N         Construir y probar quad-tree\n"
        << "  --invquad-height N       Construir InvQuad y guardarlo\n"
        << "  --index-quality N        Calidad de consulta InvQuad (0.9)\n"
        << "  --quality-zoom-min N     Primer zoom para evaluacion adicional\n"
        << "  --quality-zoom-max N     Ultimo zoom para evaluacion adicional\n"
        << "  --java-loader-compatible Reproducir el descarte del ultimo punto\n"
        << "  --help                    Mostrar ayuda\n";
}

CliOptions parse_arguments(int argc, char** argv) {
    CliOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto require_value = [&](const std::string& name) -> std::string {
            if (index + 1 >= argc) {
                throw std::invalid_argument("Falta valor para " + name);
            }
            return argv[++index];
        };
        if (arg == "--help" || arg == "-h") {
            print_help();
            std::exit(0);
        } else if (arg == "--dataset" || arg == "-s") {
            options.dataset = require_value(arg);
        } else if (arg == "--output") {
            options.output = require_value(arg);
        } else if (arg == "--rate" || arg == "-r") {
            options.rate = std::stod(require_value(arg));
        } else if (arg == "--count") {
            options.count = static_cast<std::size_t>(std::stoull(require_value(arg)));
        } else if (arg == "--delta" || arg == "-d") {
            options.delta = std::stoi(require_value(arg));
        } else if (arg == "--limit") {
            options.limit = static_cast<std::size_t>(std::stoull(require_value(arg)));
        } else if (arg == "--zoom") {
            options.zoom = std::stoi(require_value(arg));
        } else if (arg == "--center-lat") {
            options.center_latitude = std::stod(require_value(arg));
        } else if (arg == "--center-lon") {
            options.center_longitude = std::stod(require_value(arg));
        } else if (arg == "--width") {
            options.width = std::stoi(require_value(arg));
        } else if (arg == "--height") {
            options.height = std::stoi(require_value(arg));
        } else if (arg == "--index-height") {
            options.index_height = std::stoi(require_value(arg));
        } else if (arg == "--invquad-height") {
            options.invquad_height = std::stoi(require_value(arg));
        } else if (arg == "--index-quality") {
            options.index_quality = std::stod(require_value(arg));
        } else if (arg == "--quality-zoom-min") {
            options.quality_min_zoom = std::stoi(require_value(arg));
        } else if (arg == "--quality-zoom-max") {
            options.quality_max_zoom = std::stoi(require_value(arg));
        } else if (arg == "--java-loader-compatible") {
            options.java_loader_compatible = true;
        } else {
            throw std::invalid_argument("Opcion desconocida: " + arg);
        }
    }
    return options;
}

std::filesystem::path resolve_dataset_path(
    const std::filesystem::path& requested,
    const std::filesystem::path& executable) {
    if (std::filesystem::exists(requested)) {
        return requested;
    }
    if (!requested.is_relative()) {
        throw std::runtime_error("No se encontro el dataset: " + requested.string());
    }
    const std::filesystem::path executable_dir =
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
    throw std::runtime_error(
        "No se encontro el dataset predeterminado. Ruta buscada: " +
        requested.string());
}

}  // namespace

int main(int argc, char** argv) {
    try {
        CliOptions options = parse_arguments(argc, argv);
        options.dataset = resolve_dataset_path(options.dataset, argv[0]);
        const auto start = std::chrono::steady_clock::now();

        std::cout << "Configuracion:\n"
                  << "  Dataset: " << options.dataset.string() << '\n'
                  << "  Zoom de seleccion: " << options.zoom << '\n'
                  << "  Delta: " << options.delta << '\n'
                  << "  Tasa de muestreo: " << options.rate << '\n'
                  << "  Calidad multi-zoom: " << options.quality_min_zoom
                  << " a " << options.quality_max_zoom << "\n\n"
                  << "[1/5] Cargando dataset..." << std::endl;

        cheetah::Dataset dataset = cheetah::load_dataset(
            options.dataset,
            {options.limit, options.java_loader_compatible});

        std::cout << "[2/5] Proyectando y rasterizando puntos GPS..."
                  << std::endl;
        cheetah::ProjectionOptions projection_options;
        projection_options.zoom = options.zoom;
        projection_options.center_latitude = options.center_latitude;
        projection_options.center_longitude = options.center_longitude;
        projection_options.viewport_width = options.width;
        projection_options.viewport_height = options.height;
        cheetah::rasterize_dataset(
            dataset, cheetah::WebMercatorProjection(projection_options));

        std::cout << "[3/5] Ejecutando seleccion VQGS+..." << std::endl;
        cheetah::VqgsEngine engine(dataset);
        cheetah::VqgsOptions vqgs_options;
        vqgs_options.delta = options.delta;
        vqgs_options.sampling_rate = options.rate;
        vqgs_options.sample_count = options.count;
        const cheetah::VqgsResult result = engine.select(vqgs_options);
        const double selection_quality = engine.quality(result);
        std::cout << "Q(S,S') en zoom " << options.zoom << ": "
                  << std::fixed << std::setprecision(6) << selection_quality
                  << " (" << selection_quality * 100.0 << " %)" << std::endl;

        const std::vector<int> representative =
            engine.representative_scores(result, options.delta);

        std::cout << "[4/5] Guardando seleccion y representatividad..."
                  << std::endl;
        std::filesystem::create_directories(options.output);
        cheetah::save_positions(dataset, options.output / "positions.csv");
        cheetah::save_selection(result.trajectory_ids,
                                options.output / "vqgs.csv");
        cheetah::save_representativeness(result.trajectory_ids,
                                         representative,
                                         options.output / "representative.csv");

        if (options.quality_min_zoom >= 0 || options.quality_max_zoom >= 0) {
            if (options.quality_min_zoom < 0 ||
                options.quality_max_zoom < options.quality_min_zoom) {
                throw std::invalid_argument("Rango de zoom de calidad invalido");
            }
            std::ofstream quality_output(options.output / "quality_by_zoom.csv");
            quality_output << "zoom,delta,rate,quality\n";
            std::cout << "[5/5] Calidad de la seleccion fija por zoom:"
                      << std::endl;
            for (int zoom = options.quality_min_zoom;
                 zoom <= options.quality_max_zoom; ++zoom) {
                cheetah::ProjectionOptions evaluation_options = projection_options;
                evaluation_options.zoom = zoom;
                const double zoom_quality = cheetah::quality_at_projection(
                    dataset,
                    result.trajectory_ids,
                    cheetah::WebMercatorProjection(evaluation_options),
                    options.delta);
                quality_output << zoom << ',' << options.delta << ','
                               << options.rate << ',' << std::setprecision(10)
                               << zoom_quality << '\n';
                std::cout << "  zoom " << zoom << ": " << std::fixed
                          << std::setprecision(6) << zoom_quality << std::endl;
            }
        }

        if (options.index_height > 0) {
            cheetah::QuadTreeIndex index(dataset, options.index_height);
            const auto all = index.query(index.bounds());
            std::cout << "Quad-tree: " << all.size()
                      << " trayectorias recuperadas en la region total\n";
        }

        if (options.invquad_height > 0) {
            cheetah::InvQuadIndex index(
                dataset,
                cheetah::WebMercatorProjection(projection_options),
                options.invquad_height,
                options.delta);
            index.save(options.output / "invquad.csv");
            const auto indexed = index.query(index.bounds(), options.index_quality);
            cheetah::save_selection(indexed, options.output / "invquad_query.csv");
            std::cout << "InvQuad: " << indexed.size()
                      << " trayectorias para calidad " << options.index_quality << '\n';
        }

        const auto end = std::chrono::steady_clock::now();
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        std::cout << "Trayectorias cargadas: " << dataset.trajectories.size() << '\n'
                  << "Pixeles totales: " << engine.all_pixels().size() << '\n'
                  << "Trayectorias seleccionadas: " << result.trajectory_ids.size() << '\n'
                  << "Calidad visual Q(S,S'): " << std::fixed
                  << std::setprecision(6) << selection_quality << '\n'
                  << "Tiempo total (ms): " << milliseconds << '\n'
                  << "Resultados: " << std::filesystem::absolute(options.output).string()
                  << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}

#include "cheetah/invquad.hpp"
#include "cheetah/io.hpp"
#include "cheetah/temporal_invquad.hpp"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;
namespace fs = std::filesystem;

PROCESS_MEMORY_COUNTERS memory_usage() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        throw std::runtime_error("No se pudo medir la memoria del proceso");
    }
    return counters;
}

cheetah::ProjectionOptions projection_for(const cheetah::Dataset& dataset) {
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
    options.zoom = 15;
    options.center_latitude = (min_lat + max_lat) / 2;
    options.center_longitude = (min_lon + max_lon) / 2;
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    try {
#if defined(CHEETAH_OFFLINE_STATIC)
        constexpr int offset = 0;
        constexpr const char* fixed_mode = "static";
#elif defined(CHEETAH_OFFLINE_TEMPORAL)
        constexpr int offset = 0;
        constexpr const char* fixed_mode = "temporal";
#else
        constexpr int offset = 1;
        constexpr const char* fixed_mode = nullptr;
#endif
        constexpr bool show_progress = offset == 0;
        if (argc != 4 + offset) {
            if (show_progress) {
                throw std::invalid_argument(std::string("Uso: ") + argv[0] +
                                            " TRAIN LIMITE SALIDA_NUEVA");
            }
            throw std::invalid_argument(
                "Uso: cheetahtraj-offline-benchmark.exe static|temporal TRAIN LIMITE SALIDA_NUEVA");
        }
        const std::string mode = show_progress ? fixed_mode : argv[1];
        if (mode != "static" && mode != "temporal") {
            throw std::invalid_argument("Modo desconocido");
        }
        const std::string limit = argv[2 + offset];
        if (limit.empty() || limit.find_first_not_of("0123456789") != std::string::npos) {
            throw std::invalid_argument("Limite de viajes invalido");
        }
        const auto count = std::stoull(limit);
        if (count == 0 || count > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("El limite debe ser positivo");
        }
        const fs::path output = argv[3 + offset];
        if (fs::exists(output)) throw std::invalid_argument("La salida ya existe");

        cheetah::TemporalIndexOptions options;
        options.height = 2;
        options.delta = 4;
        cheetah::PortoLoadOptions loader;
        loader.limit = static_cast<std::size_t>(count);
        loader.minimum_points = options.minimum_points;
        if (show_progress) {
            std::cout << std::fixed << std::setprecision(2)
                      << "Metodo: " << (mode == "static" ? "InvQuad estatico" : "InvQuad temporal")
                      << "\nZoom 15 | delta 4 | altura 2"
                      << (mode == "temporal" ? " | ventanas de 3600 segundos" : "")
                      << "\n[1/3] Leyendo hasta " << count << " viajes de train.csv..." << std::endl;
        }
        const auto before_load = Clock::now();
        const auto dataset = cheetah::load_porto_train_dataset(argv[1 + offset], loader);
        const auto after_load = Clock::now();
        if (dataset.trajectories.size() != loader.limit) {
            throw std::runtime_error("train.csv no contiene el numero solicitado de viajes validos");
        }
        options.projection = projection_for(dataset);
        const cheetah::WebMercatorProjection projection(options.projection);
        const auto before_memory = memory_usage();

        if (show_progress) {
            std::cout << "      Viajes cargados: " << dataset.trajectories.size()
                      << " | Lectura: " << Milliseconds(after_load - before_load).count()
                      << " ms\n[2/3] Fase offline: "
                      << (mode == "static" ? "construyendo el arbol global..."
                                           : "preparando las ventanas...")
                      << std::endl;
        }
        cheetah::TemporalIndexBuildResult result;
        const auto offline_start = Clock::now();
        if (mode == "static") {
            const auto before_build = Clock::now();
            const cheetah::InvQuadIndex index(dataset, projection, options.height, options.delta);
            const auto after_build = Clock::now();
            if (show_progress) std::cout << "      Guardando invquad.csv..." << std::endl;
            const auto before_save = Clock::now();
            index.save(output / "invquad.csv");
            const auto after_save = Clock::now();
            result.invquad_build_ms = Milliseconds(after_build - before_build).count();
            result.invquad_save_ms = Milliseconds(after_save - before_save).count();
            result.stored_segments = dataset.trajectories.size();
        } else {
            result = cheetah::TemporalInvQuadIndex::build(
                dataset, output, options, show_progress ? &std::cout : nullptr);
        }
        const auto offline_end = Clock::now();
        const auto after_memory = memory_usage();

        std::uintmax_t index_bytes = 0;
        std::uintmax_t output_bytes = 0;
        for (const auto& entry : fs::directory_iterator(output)) {
            if (!entry.is_regular_file()) continue;
            const auto bytes = entry.file_size();
            output_bytes += bytes;
            if (entry.path().filename().string().find("invquad.csv") != std::string::npos) {
                index_bytes += bytes;
            }
        }

        if (show_progress) {
            constexpr double bytes_per_mib = 1024.0 * 1024.0;
            std::cout << "[3/3] Resultados\n"
                      << "      Arboles construidos: "
                      << (mode == "static" ? 1 : result.nonempty_windows)
                      << "\n      Construccion InvQuad: " << result.invquad_build_ms << " ms"
                      << "\n      Guardado InvQuad: " << result.invquad_save_ms << " ms"
                      << "\n      Offline total: " << Milliseconds(offline_end - offline_start).count() << " ms"
                      << "\n      RAM pico del proceso: " << after_memory.PeakWorkingSetSize / bytes_per_mib << " MiB"
                      << "\n      Solo indices: " << index_bytes << " bytes"
                      << "\n      Salida total: " << output_bytes << " bytes"
                      << "\n      Carpeta: " << output << '\n';
            return 0;
        }

        std::cout << std::fixed << std::setprecision(4)
                  << "{\"mode\":\"" << mode
                  << "\",\"trips\":" << dataset.trajectories.size()
                  << ",\"windows\":" << result.windows
                  << ",\"nonempty_windows\":" << result.nonempty_windows
                  << ",\"segments\":" << result.stored_segments
                  << ",\"load_ms\":" << Milliseconds(after_load - before_load).count()
                  << ",\"invquad_build_ms\":" << result.invquad_build_ms
                  << ",\"invquad_save_ms\":" << result.invquad_save_ms
                  << ",\"offline_ms\":" << Milliseconds(offline_end - offline_start).count()
                  << ",\"ram_before_offline_bytes\":" << before_memory.WorkingSetSize
                  << ",\"ram_peak_before_offline_bytes\":" << before_memory.PeakWorkingSetSize
                  << ",\"ram_peak_bytes\":" << after_memory.PeakWorkingSetSize
                  << ",\"index_bytes\":" << index_bytes
                  << ",\"output_bytes\":" << output_bytes << "}\n";
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}

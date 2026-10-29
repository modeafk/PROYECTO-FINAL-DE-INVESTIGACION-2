#include "cheetah/io.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace cheetah {
namespace {

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::stringstream stream(value);
    std::string part;
    while (std::getline(stream, part, delimiter)) {
        parts.push_back(part);
    }
    return parts;
}

std::vector<std::string> parse_csv_row(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (character == '"') {
            if (quoted && index + 1 < line.size() && line[index + 1] == '"') {
                field.push_back('"');
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (character == ',' && !quoted) {
            fields.push_back(std::move(field));
            field.clear();
        } else {
            field.push_back(character);
        }
    }
    if (quoted) {
        throw std::runtime_error("Fila CSV con comillas sin cerrar");
    }
    fields.push_back(std::move(field));
    return fields;
}

bool parse_boolean(const std::string& value) {
    return value == "True" || value == "true" || value == "1";
}

std::vector<Location> parse_porto_polyline(const std::string& polyline,
                                           Timestamp trip_start,
                                           const PortoLoadOptions& options) {
    std::vector<Location> locations;
    const char* cursor = polyline.c_str();
    const char* end = cursor + polyline.size();
    std::size_t point_index = 0;
    while (cursor < end) {
        while (cursor < end &&
               !(*cursor == '-' || *cursor == '+' ||
                 (*cursor >= '0' && *cursor <= '9'))) {
            ++cursor;
        }
        if (cursor >= end) {
            break;
        }
        char* longitude_end = nullptr;
        const double longitude = std::strtod(cursor, &longitude_end);
        if (longitude_end == cursor) {
            throw std::runtime_error("Longitud invalida en POLYLINE");
        }
        cursor = longitude_end;
        while (cursor < end &&
               !(*cursor == '-' || *cursor == '+' ||
                 (*cursor >= '0' && *cursor <= '9'))) {
            ++cursor;
        }
        if (cursor >= end) {
            throw std::runtime_error("Latitud ausente en POLYLINE");
        }
        char* latitude_end = nullptr;
        const double latitude = std::strtod(cursor, &latitude_end);
        if (latitude_end == cursor) {
            throw std::runtime_error("Latitud invalida en POLYLINE");
        }
        cursor = latitude_end;

        if (point_index > static_cast<std::size_t>(
                (std::numeric_limits<Timestamp>::max() - trip_start) /
                options.point_interval_seconds)) {
            throw std::overflow_error("Timestamp de punto fuera de rango");
        }
        const Timestamp timestamp =
            trip_start + static_cast<Timestamp>(point_index) *
                             options.point_interval_seconds;
        const bool after_start = !options.start_time ||
                                 timestamp >= *options.start_time;
        const bool before_end = !options.end_time ||
                                timestamp < *options.end_time;
        if (after_start && before_end) {
            locations.push_back({longitude, latitude, timestamp});
        }
        ++point_index;
    }
    return locations;
}

void ensure_parent(const std::filesystem::path& path) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
}

}  // namespace

Dataset load_dataset(const std::filesystem::path& path, const LoadOptions& options) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("No se pudo abrir el dataset: " + path.string());
    }

    Dataset dataset;
    dataset.source_path = path.string();
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        if (options.limit != 0 && dataset.trajectories.size() >= options.limit) {
            break;
        }
        ++line_number;

        const auto semicolon = line.rfind(';');
        const std::string coordinate_text =
            semicolon == std::string::npos ? line : line.substr(semicolon + 1);
        const auto fields = split(coordinate_text, ',');
        if (fields.size() < 2 || fields.size() % 2 != 0) {
            throw std::runtime_error("Formato invalido en la linea " +
                                     std::to_string(line_number));
        }

        Trajectory trajectory;
        trajectory.id = static_cast<int>(dataset.trajectories.size());
        trajectory.source_id = trajectory.id;
        std::size_t coordinate_count = fields.size();
        if (options.java_loader_compatible && coordinate_count >= 2) {
            coordinate_count -= 2;
        }
        trajectory.locations.reserve(coordinate_count / 2);

        for (std::size_t index = 0; index + 1 < coordinate_count; index += 2) {
            trajectory.locations.push_back({
                std::stod(fields[index]),
                std::stod(fields[index + 1])
            });
        }
        dataset.trajectories.push_back(std::move(trajectory));
    }

    return dataset;
}

Dataset load_temporal_dataset(const std::filesystem::path& path,
                              const LoadOptions& options) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("No se pudo abrir el dataset temporal: " +
                                 path.string());
    }

    Dataset dataset;
    dataset.source_path = path.string();
    std::unordered_map<std::int64_t, std::size_t> source_to_local;
    std::string line;
    std::size_t line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto fields = split(line, ',');
        if (fields.size() != 4) {
            throw std::runtime_error(
                "Formato temporal invalido en la linea " +
                std::to_string(line_number) +
                "; se esperaban 4 columnas");
        }

        std::int64_t source_id{};
        double longitude{};
        double latitude{};
        Timestamp timestamp{};
        try {
            source_id = std::stoll(fields[0]);
            longitude = std::stod(fields[1]);
            latitude = std::stod(fields[2]);
            timestamp = std::stoll(fields[3]);
        } catch (const std::exception&) {
            // An optional header is accepted only on the first physical line.
            if (line_number == 1) {
                continue;
            }
            throw std::runtime_error(
                "Valor temporal invalido en la linea " +
                std::to_string(line_number));
        }

        auto found = source_to_local.find(source_id);
        if (found == source_to_local.end()) {
            if (options.limit != 0 &&
                dataset.trajectories.size() >= options.limit) {
                continue;
            }
            Trajectory trajectory;
            trajectory.id = static_cast<int>(dataset.trajectories.size());
            trajectory.source_id = source_id;
            dataset.trajectories.push_back(std::move(trajectory));
            const std::size_t local = dataset.trajectories.size() - 1;
            source_to_local.emplace(source_id, local);
            found = source_to_local.find(source_id);
        }
        dataset.trajectories[found->second].locations.push_back(
            {longitude, latitude, timestamp});
    }

    for (auto& trajectory : dataset.trajectories) {
        std::stable_sort(
            trajectory.locations.begin(), trajectory.locations.end(),
            [](const Location& left, const Location& right) {
                return left.timestamp < right.timestamp;
            });
    }
    return dataset;
}

Dataset load_porto_train_dataset(const std::filesystem::path& path,
                                 const PortoLoadOptions& options) {
    if (options.point_interval_seconds <= 0) {
        throw std::invalid_argument("El intervalo de puntos debe ser positivo");
    }
    if (options.start_time && options.end_time &&
        *options.end_time <= *options.start_time) {
        throw std::invalid_argument("El rango temporal de Porto es invalido");
    }

    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("No se pudo abrir train.csv de Porto: " +
                                 path.string());
    }
    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("train.csv esta vacio");
    }
    const auto header = parse_csv_row(line);
    std::unordered_map<std::string, std::size_t> columns;
    for (std::size_t index = 0; index < header.size(); ++index) {
        columns.emplace(header[index], index);
    }
    const char* required[] = {"TRIP_ID", "TIMESTAMP", "MISSING_DATA", "POLYLINE"};
    for (const char* name : required) {
        if (columns.find(name) == columns.end()) {
            throw std::runtime_error(std::string("Columna ausente: ") + name);
        }
    }

    Dataset dataset;
    dataset.source_path = path.string();
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) {
            continue;
        }
        const auto fields = parse_csv_row(line);
        if (fields.size() != header.size()) {
            throw std::runtime_error("Cantidad de columnas invalida en la linea " +
                                     std::to_string(line_number));
        }
        if (options.skip_missing_data &&
            parse_boolean(fields[columns.at("MISSING_DATA")])) {
            continue;
        }

        const std::int64_t source_id =
            std::stoll(fields[columns.at("TRIP_ID")]);
        const Timestamp trip_start =
            std::stoll(fields[columns.at("TIMESTAMP")]);
        std::vector<Location> locations = parse_porto_polyline(
            fields[columns.at("POLYLINE")], trip_start, options);
        if (locations.size() < options.minimum_points) {
            continue;
        }

        Trajectory trajectory;
        trajectory.id = static_cast<int>(dataset.trajectories.size());
        trajectory.source_id = source_id;
        trajectory.locations = std::move(locations);
        dataset.trajectories.push_back(std::move(trajectory));
        if (options.limit != 0 &&
            dataset.trajectories.size() >= options.limit) {
            break;
        }
    }
    return dataset;
}

void save_positions(const Dataset& dataset, const std::filesystem::path& path) {
    ensure_parent(path);
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("No se pudo escribir: " + path.string());
    }
    for (const auto& trajectory : dataset.trajectories) {
        for (std::size_t index = 0; index < trajectory.pixels.size(); ++index) {
            if (index != 0) {
                output << ',';
            }
            output << trajectory.pixels[index].x << ',' << trajectory.pixels[index].y;
        }
        output << '\n';
    }
}

void save_selection(const std::vector<int>& trajectory_ids,
                    const std::filesystem::path& path) {
    ensure_parent(path);
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("No se pudo escribir: " + path.string());
    }
    for (int id : trajectory_ids) {
        output << id << ",0\n";
    }
}

void save_representativeness(const std::vector<int>& trajectory_ids,
                             const std::vector<int>& scores,
                             const std::filesystem::path& path) {
    if (trajectory_ids.size() != scores.size()) {
        throw std::invalid_argument("IDs y puntajes tienen tamanos distintos");
    }
    ensure_parent(path);
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("No se pudo escribir: " + path.string());
    }
    for (std::size_t index = 0; index < trajectory_ids.size(); ++index) {
        output << trajectory_ids[index] << ',' << scores[index] << '\n';
    }
}

}  // namespace cheetah

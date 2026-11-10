#ifdef _WIN32

#include "cheetah/io.hpp"
#include "cheetah/temporal_invquad.hpp"
#include "cheetah/vqgs.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

struct WindowInfo {
    std::size_t id{};
    cheetah::Timestamp start{};
    cheetah::Timestamp end{};
    std::size_t segments{};
};

struct Panel {
    WindowInfo info;
    cheetah::TemporalQueryResult result;
    cheetah::Dataset selected;
    std::vector<int> representative_scores;
};

struct IndexSettings {
    cheetah::ProjectionOptions projection;
    int delta{};
};

struct Bounds {
    double min_lat{std::numeric_limits<double>::max()};
    double max_lat{std::numeric_limits<double>::lowest()};
    double min_lon{std::numeric_limits<double>::max()};
    double max_lon{std::numeric_limits<double>::lowest()};
};

struct AppState {
    std::vector<Panel> panels;
    Bounds full;
    Bounds robust;
    bool robust_extent{true};
    bool color_by_representativeness{false};
    int maximum_representative_score{1};
    double zoom{1.0};
    double pan_x{};
    double pan_y{};
};

AppState app;

std::string format_utc(cheetah::Timestamp timestamp) {
    const auto seconds = static_cast<std::time_t>(timestamp);
    std::tm calendar{};
    if (gmtime_s(&calendar, &seconds) != 0)
        throw std::runtime_error("Timestamp UTC invalido");
    std::ostringstream out;
    out << std::put_time(&calendar, "%Y-%m-%d %H:%M");
    return out.str();
}

std::vector<WindowInfo> read_windows(const std::filesystem::path& directory) {
    std::ifstream input(directory / "manifest.csv");
    std::string line;
    if (!input || !std::getline(input, line) || line != "# temporal_invquad_v1")
        throw std::runtime_error("No se encontro un indice temporal valido: " +
                                 directory.string());
    for (int i = 0; i < 3; ++i)
        if (!std::getline(input, line))
            throw std::runtime_error("Manifest temporal incompleto");
    std::vector<WindowInfo> windows;
    while (std::getline(input, line)) {
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream row(line);
        WindowInfo window;
        if (!(row >> window.id >> window.start >> window.end >> window.segments))
            throw std::runtime_error("Fila invalida en manifest.csv");
        windows.push_back(window);
    }
    if (windows.empty()) throw std::runtime_error("El indice no contiene ventanas");
    return windows;
}

IndexSettings read_index_settings(const std::filesystem::path& directory) {
    std::ifstream input(directory / "manifest.csv");
    std::string line;
    if (!input || !std::getline(input, line) ||
        line != "# temporal_invquad_v1" ||
        !std::getline(input, line) || !std::getline(input, line)) {
        throw std::runtime_error("Manifest temporal incompleto");
    }
    std::replace(line.begin(), line.end(), ',', ' ');
    std::istringstream row(line);
    IndexSettings settings;
    int tree_height = 0;
    std::size_t minimum_points = 0;
    if (!(row >> settings.projection.zoom >>
          settings.projection.center_latitude >>
          settings.projection.center_longitude >>
          settings.projection.viewport_width >>
          settings.projection.viewport_height >> tree_height >>
          settings.delta >> minimum_points) || settings.delta < 0) {
        throw std::runtime_error("Parametros invalidos en manifest.csv");
    }
    return settings;
}

void add(Bounds& bounds, const cheetah::Location& point) {
    bounds.min_lat = std::min(bounds.min_lat, point.latitude);
    bounds.max_lat = std::max(bounds.max_lat, point.latitude);
    bounds.min_lon = std::min(bounds.min_lon, point.longitude);
    bounds.max_lon = std::max(bounds.max_lon, point.longitude);
}

Bounds shared_bounds(const std::vector<cheetah::Dataset>& datasets) {
    Bounds bounds;
    for (const auto& dataset : datasets)
        for (const auto& trajectory : dataset.trajectories)
            for (const auto& point : trajectory.locations) add(bounds, point);
    if (bounds.min_lat > bounds.max_lat)
        throw std::runtime_error("Las ventanas elegidas no tienen puntos GPS");
    return bounds;
}

Bounds robust_bounds(const std::vector<cheetah::Dataset>& datasets,
                     const Bounds& fallback) {
    std::vector<double> latitudes, longitudes;
    for (const auto& dataset : datasets)
        for (const auto& trajectory : dataset.trajectories)
            for (const auto& point : trajectory.locations) {
                latitudes.push_back(point.latitude);
                longitudes.push_back(point.longitude);
            }
    if (latitudes.size() < 200) return fallback;
    std::sort(latitudes.begin(), latitudes.end());
    std::sort(longitudes.begin(), longitudes.end());
    const auto percentile = [](const std::vector<double>& values, double p) {
        return values[static_cast<std::size_t>(p * (values.size() - 1))];
    };
    return {percentile(latitudes, 0.005), percentile(latitudes, 0.995),
            percentile(longitudes, 0.005), percentile(longitudes, 0.995)};
}

cheetah::GeoRect to_region(const Bounds& bounds) {
    return {bounds.min_lat, bounds.max_lat, bounds.min_lon, bounds.max_lon};
}

std::vector<Panel> query_panels(const std::filesystem::path& directory,
                                const std::vector<WindowInfo>& chosen,
                                double quality_target) {
    const auto settings = read_index_settings(directory);
    const cheetah::WebMercatorProjection projection(settings.projection);
    std::vector<cheetah::Dataset> windows;
    for (const auto& info : chosen)
        windows.push_back(cheetah::load_temporal_dataset(
            directory / ("window_" + std::to_string(info.id) + ".csv")));
    app.full = shared_bounds(windows);
    app.robust = robust_bounds(windows, app.full);
    const auto region = to_region(app.full);
    std::vector<Panel> panels;
    for (std::size_t i = 0; i < chosen.size(); ++i) {
        Panel panel;
        panel.info = chosen[i];
        panel.result = cheetah::TemporalInvQuadIndex::query(
            directory, chosen[i].id, region, quality_target);
        const std::unordered_set<std::int64_t> ids(
            panel.result.source_ids.begin(), panel.result.source_ids.end());
        cheetah::rasterize_dataset(windows[i], projection);
        cheetah::VqgsResult selection;
        for (const auto& trajectory : windows[i].trajectories)
            if (ids.count(trajectory.source_id))
                selection.trajectory_ids.push_back(trajectory.id);
        panel.representative_scores =
            cheetah::VqgsEngine(windows[i]).representative_scores(
                selection, settings.delta);
        for (int score : panel.representative_scores)
            app.maximum_representative_score =
                std::max(app.maximum_representative_score, score);
        for (auto& trajectory : windows[i].trajectories)
            if (ids.count(trajectory.source_id))
                panel.selected.trajectories.push_back(std::move(trajectory));
        panels.push_back(std::move(panel));
    }
    return panels;
}

RECT panel_area(const RECT& client, std::size_t index, std::size_t count) {
    const int width = client.right - client.left;
    const int left = static_cast<int>(index) * width / static_cast<int>(count);
    const int right = static_cast<int>(index + 1) * width / static_cast<int>(count);
    return {left, 72, right, client.bottom - 30};
}

POINT to_screen(const cheetah::Location& point, const RECT& area) {
    const Bounds& bounds = app.robust_extent ? app.robust : app.full;
    const double lon_span = std::max(1e-9, bounds.max_lon - bounds.min_lon);
    const double lat_span = std::max(1e-9, bounds.max_lat - bounds.min_lat);
    const double width = std::max<LONG>(1, area.right - area.left - 48);
    const double height = std::max<LONG>(1, area.bottom - area.top - 48);
    // Both panels use the same geographic extent and pixels-per-degree.
    const double scale = std::min(width / lon_span, height / lat_span) * app.zoom;
    const double center_x = (area.left + area.right) / 2.0 + app.pan_x;
    const double center_y = (area.top + area.bottom) / 2.0 + app.pan_y;
    const double mid_lon = (bounds.min_lon + bounds.max_lon) / 2.0;
    const double mid_lat = (bounds.min_lat + bounds.max_lat) / 2.0;
    return {static_cast<LONG>(std::lround(center_x + (point.longitude - mid_lon) * scale)),
            static_cast<LONG>(std::lround(center_y - (point.latitude - mid_lat) * scale))};
}

void label(HDC dc, int x, int y, const std::string& value) {
    TextOutA(dc, x, y, value.c_str(), static_cast<int>(value.size()));
}

COLORREF representative_color(int score) {
    const double denominator = std::log1p(
        static_cast<double>(app.maximum_representative_score));
    const double value = denominator > 0.0
        ? std::log1p(static_cast<double>(std::max(0, score))) / denominator
        : 0.0;
    return RGB(static_cast<int>(30 + 210 * value),
               static_cast<int>(130 - 70 * value),
               static_cast<int>(200 - 160 * value));
}

void draw_panel(HDC dc, const RECT& area, const Panel& panel, COLORREF color) {
    HBRUSH background = CreateSolidBrush(RGB(250, 252, 254));
    FillRect(dc, &area, background);
    DeleteObject(background);
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
    for (std::size_t index = 0; index < panel.selected.trajectories.size(); ++index) {
        const auto& trajectory = panel.selected.trajectories[index];
        if (trajectory.locations.empty()) continue;
        const COLORREF trajectory_color = app.color_by_representativeness &&
                                         index < panel.representative_scores.size()
            ? representative_color(panel.representative_scores[index]) : color;
        HPEN pen = CreatePen(PS_SOLID, 2, trajectory_color);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        POINT first = to_screen(trajectory.locations.front(), area);
        if (trajectory.locations.size() == 1) {
            SetPixel(dc, first.x, first.y, trajectory_color);
        } else {
            MoveToEx(dc, first.x, first.y, nullptr);
            for (std::size_t i = 1; i < trajectory.locations.size(); ++i) {
                const POINT next = to_screen(trajectory.locations[i], area);
                LineTo(dc, next.x, next.y);
            }
        }
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }
    RestoreDC(dc, saved);
}

void draw_scene(HDC dc, const RECT& client) {
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    label(dc, 15, 12, app.panels.size() == 1
        ? "CheetahTraj temporal | una ventana" :
          "CheetahTraj temporal | comparacion de ventanas (misma escala)");
    label(dc, 15, 36,
          "C: color por representatividad  |  Rueda: zoom  |  Flechas: mover  |  F: encuadre  |  R: reiniciar");
    for (std::size_t i = 0; i < app.panels.size(); ++i) {
        const RECT area = panel_area(client, i, app.panels.size());
        const auto& panel = app.panels[i];
        draw_panel(dc, area, panel, i == 0 ? RGB(21, 108, 170) : RGB(205, 90, 51));
        RECT panel_header{area.left, area.top, area.right, area.top + 32};
        FillRect(dc, &panel_header,
                 static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        std::ostringstream status;
        status << "W" << panel.info.id << "  " << format_utc(panel.info.start)
               << " a " << format_utc(panel.info.end) << " UTC"
               << "  |  " << panel.selected.trajectories.size() << "/"
               << panel.info.segments << " segmentos"
               << "  |  Q=" << std::fixed << std::setprecision(3)
               << panel.result.quality;
        if (app.color_by_representativeness) status << "  |  color: representatividad";
        label(dc, area.left + 12, area.top + 10, status.str());
        if (i + 1 < app.panels.size()) {
            HPEN divider = CreatePen(PS_SOLID, 1, RGB(190, 200, 210));
            HGDIOBJ old = SelectObject(dc, divider);
            MoveToEx(dc, area.right, area.top, nullptr);
            LineTo(dc, area.right, area.bottom);
            SelectObject(dc, old);
            DeleteObject(divider);
        }
    }
    if (app.color_by_representativeness) {
        std::ostringstream legend;
        legend << "Color: azul = menos trayectorias representadas; rojo = mas (maximo: "
               << app.maximum_representative_score << " por trayectoria seleccionada)";
        label(dc, 15, client.bottom - 23, legend.str());
    } else {
        label(dc, 15, client.bottom - 23,
              app.robust_extent ? "Encuadre principal: omite el 0.5% extremo de puntos (F muestra todo)" :
                                  "Encuadre completo: incluye todos los puntos GPS (F vuelve al principal)");
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            if (client.right > 0 && client.bottom > 0) {
                HDC buffer_dc = CreateCompatibleDC(dc);
                HBITMAP buffer = CreateCompatibleBitmap(dc, client.right, client.bottom);
                HGDIOBJ old = SelectObject(buffer_dc, buffer);
                draw_scene(buffer_dc, client);
                BitBlt(dc, 0, 0, client.right, client.bottom, buffer_dc, 0, 0, SRCCOPY);
                SelectObject(buffer_dc, old);
                DeleteObject(buffer);
                DeleteDC(buffer_dc);
            }
            EndPaint(window, &paint);
            return 0;
        }
        case WM_MOUSEWHEEL:
            app.zoom = std::clamp(app.zoom * (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ?
                                              1.2 : 1.0 / 1.2), 0.5, 40.0);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (wparam == 'R') { app.zoom = 1.0; app.pan_x = app.pan_y = 0.0; }
            else if (wparam == 'C')
                app.color_by_representativeness = !app.color_by_representativeness;
            else if (wparam == 'F') {
                app.robust_extent = !app.robust_extent;
                app.zoom = 1.0;
                app.pan_x = app.pan_y = 0.0;
            } else if (wparam == VK_LEFT) app.pan_x -= 30;
            else if (wparam == VK_RIGHT) app.pan_x += 30;
            else if (wparam == VK_UP) app.pan_y -= 30;
            else if (wparam == VK_DOWN) app.pan_y += 30;
            else break;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_SIZE:
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void show_window() {
    const wchar_t* class_name = L"CheetahTrajTemporalVisual";
    WNDCLASSW klass{};
    klass.lpfnWndProc = window_proc;
    klass.hInstance = GetModuleHandleW(nullptr);
    klass.lpszClassName = class_name;
    klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("No se pudo registrar la ventana grafica");
    HWND window = CreateWindowExW(0, class_name, L"CheetahTraj - ventanas temporales",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1400, 850,
        nullptr, nullptr, klass.hInstance, nullptr);
    if (!window) throw std::runtime_error("No se pudo abrir la ventana grafica");
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

std::size_t ask_window(const std::vector<WindowInfo>& windows,
                       const std::string& prompt) {
    std::cout << prompt;
    std::string line;
    if (!std::getline(std::cin, line)) throw std::runtime_error("Entrada finalizada");
    std::size_t consumed = 0;
    const auto number = std::stoull(line, &consumed);
    if (consumed != line.size()) throw std::invalid_argument("Numero invalido");
    const auto found = std::find_if(windows.begin(), windows.end(),
        [&](const WindowInfo& item) { return item.id == number; });
    if (found == windows.end()) throw std::out_of_range("Esa ventana no existe");
    if (found->segments == 0) throw std::invalid_argument("Ventana vacia");
    return static_cast<std::size_t>(found - windows.begin());
}

void run(const std::filesystem::path& directory) {
    const auto windows = read_windows(directory);
    std::cout << "Indice: " << directory << "\n";
    while (true) {
        std::cout << "\nVentanas disponibles (UTC):\n";
        for (const auto& item : windows)
            std::cout << "  W" << item.id << "  " << format_utc(item.start)
                      << " a " << format_utc(item.end) << "  ("
                      << item.segments << " segmentos)\n";
        std::cout << "\n1 = visualizar una ventana, 2 = comparar dos ventanas, q = salir: ";
        std::string choice;
        if (!std::getline(std::cin, choice) || choice == "q" || choice == "Q") return;
        if (choice != "1" && choice != "2") {
            std::cout << "Escribe 1, 2 o q.\n";
            continue;
        }
        try {
            std::vector<WindowInfo> chosen;
            chosen.push_back(windows[ask_window(windows, "Primera ventana (numero): ")]);
            if (choice == "2") {
                const auto second = windows[ask_window(windows, "Segunda ventana (numero): ")];
                if (chosen.front().id == second.id)
                    throw std::invalid_argument("Elige dos ventanas distintas");
                chosen.push_back(second);
            }
            std::cout << "Calidad objetivo Q (Enter = 0.90): ";
            std::string line;
            if (!std::getline(std::cin, line)) return;
            const double target = line.empty() ? 0.9 : std::stod(line);
            if (!(target > 0.0 && target <= 1.0))
                throw std::invalid_argument("Q debe estar en (0,1]");
            std::cout << "Consultando el indice temporal...\n";
            app = AppState{};
            app.panels = query_panels(directory, chosen, target);
            for (const auto& panel : app.panels)
                std::cout << "W" << panel.info.id << ": "
                          << panel.selected.trajectories.size() << "/"
                          << panel.info.segments << " segmentos, Q="
                          << std::fixed << std::setprecision(6)
                          << panel.result.quality << "\n";
            std::cout << "Cierra la ventana grafica para volver al menu.\n";
            show_window();
        } catch (const std::exception& error) {
            std::cout << "No se pudo visualizar: " << error.what() << "\n";
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path directory = "output/porto_st_index_1k";
        if (argc == 3 && std::string(argv[1]) == "--index-dir") directory = argv[2];
        else if (argc != 1)
            throw std::invalid_argument("Uso: cheetahtraj-temporal-visual.exe [--index-dir RUTA]");
        run(directory);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}

#else
#include <iostream>
int main() {
    std::cerr << "El visor temporal requiere Windows.\n";
    return 1;
}
#endif

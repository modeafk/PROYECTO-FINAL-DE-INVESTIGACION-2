#ifdef _WIN32

#include "cheetah/io.hpp"
#include "cheetah/temporal_invquad.hpp"
#include "cheetah/vqgs.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
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

struct MapView {
    double zoom{1.0};
    double pan_x{};
    double pan_y{};
};

struct Panel {
    WindowInfo info;
    cheetah::TemporalQueryResult result;
    std::size_t available_segments{};
    cheetah::Dataset selected;
    std::vector<int> representative_scores;
    MapView view;
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
    Bounds overview;
    bool restricted_to_region{false};
    bool full_extent{false};
    std::size_t active_panel{};
    bool color_by_representativeness{false};
    bool show_basemap{true};
    int maximum_representative_score{1};
};

AppState app;

enum class MapLayer : std::size_t { Water, River, Local, Main, Motorway, Count };

struct MapPath {
    std::vector<cheetah::Location> points;
    cheetah::GeoRect bounds;
};

using Basemap = std::array<std::vector<MapPath>, static_cast<std::size_t>(MapLayer::Count)>;
Basemap basemap;
bool basemap_loaded{};
constexpr Bounds kPortoMapBounds{40.99, 41.30, -8.81, -8.42};

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

Basemap read_basemap(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string line;
    if (!std::getline(input, line) || line != "# cheetahtraj_basemap_v1") {
        throw std::runtime_error("No se pudo leer el mapa base: " + path.string());
    }
    Basemap layers;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') continue;
        std::istringstream row(line);
        std::size_t layer{};
        if (!(row >> layer) || layer >= layers.size()) {
            throw std::runtime_error("Capa invalida en el mapa base");
        }
        MapPath feature;
        Bounds bounds;
        cheetah::Location point;
        while (row >> point.longitude) {
            if (!(row >> point.latitude) || !std::isfinite(point.longitude) ||
                !std::isfinite(point.latitude)) {
                throw std::runtime_error("Coordenada invalida en el mapa base");
            }
            feature.points.push_back(point);
            add(bounds, point);
        }
        if (!row.eof() || feature.points.size() < 2) {
            throw std::runtime_error("Geometria invalida en el mapa base");
        }
        feature.bounds = to_region(bounds);
        layers[layer].push_back(std::move(feature));
    }
    return layers;
}

void keep_points_in_region(cheetah::Dataset& dataset, const cheetah::GeoRect& region) {
    for (auto& trajectory : dataset.trajectories) {
        auto& points = trajectory.locations;
        points.erase(std::remove_if(points.begin(), points.end(),
            [&](const cheetah::Location& point) { return !region.contains(point); }), points.end());
    }
    auto& trajectories = dataset.trajectories;
    trajectories.erase(std::remove_if(trajectories.begin(), trajectories.end(),
        [](const cheetah::Trajectory& trajectory) { return trajectory.locations.empty(); }),
        trajectories.end());
    for (std::size_t i = 0; i < trajectories.size(); ++i)
        trajectories[i].id = static_cast<int>(i);
}

std::vector<Panel> query_panels(const std::filesystem::path& directory,
                                const std::vector<WindowInfo>& chosen,
                                double quality_target,
                                const std::optional<cheetah::GeoRect>& requested_region = std::nullopt) {
    const auto settings = read_index_settings(directory);
    const cheetah::WebMercatorProjection projection(settings.projection);
    std::vector<cheetah::Dataset> windows;
    for (const auto& info : chosen)
        windows.push_back(cheetah::load_temporal_dataset(
            directory / ("window_" + std::to_string(info.id) + ".csv")));
    app.restricted_to_region = requested_region.has_value();
    if (requested_region) {
        app.full = {requested_region->min_latitude, requested_region->max_latitude,
                    requested_region->min_longitude, requested_region->max_longitude};
        app.overview = app.full;
    } else {
        app.full = shared_bounds(windows);
        app.overview = robust_bounds(windows, app.full);
        if (basemap_loaded && to_region(app.overview).intersects(to_region(kPortoMapBounds))) {
            add(app.overview, {kPortoMapBounds.min_lon, kPortoMapBounds.min_lat});
            add(app.overview, {kPortoMapBounds.max_lon, kPortoMapBounds.max_lat});
        }
    }
    const auto region = to_region(app.full);
    std::vector<Panel> panels;
    for (std::size_t i = 0; i < chosen.size(); ++i) {
        Panel panel;
        panel.info = chosen[i];
        auto& window = windows[i];
        if (requested_region) keep_points_in_region(window, region);
        panel.available_segments = window.trajectories.size();
        if (panel.available_segments == 0) {
            panels.push_back(std::move(panel));
            continue;
        }
        panel.result = cheetah::TemporalInvQuadIndex::query(
            directory, chosen[i].id, region, quality_target);
        const std::unordered_set<std::int64_t> ids(
            panel.result.source_ids.begin(), panel.result.source_ids.end());
        cheetah::rasterize_dataset(window, projection);
        cheetah::VqgsResult selection;
        for (const auto& trajectory : window.trajectories)
            if (ids.count(trajectory.source_id))
                selection.trajectory_ids.push_back(trajectory.id);
        panel.representative_scores =
            cheetah::VqgsEngine(window).representative_scores(
                selection, settings.delta);
        for (int score : panel.representative_scores)
            app.maximum_representative_score =
                std::max(app.maximum_representative_score, score);
        for (auto& trajectory : window.trajectories)
            if (ids.count(trajectory.source_id))
                panel.selected.trajectories.push_back(std::move(trajectory));
        panels.push_back(std::move(panel));
    }
    return panels;
}

constexpr LONG kPanelTop = 110;
constexpr LONG kHeaderHeight = 44;

RECT panel_area(const RECT& client, std::size_t index, std::size_t count) {
    const LONG width = (client.right - client.left) / static_cast<LONG>(count);
    const LONG left = client.left + static_cast<LONG>(index) * width;
    return {left, kPanelTop, left + width, client.bottom - 30};
}

struct ScreenTransform {
    double scale;
    double center_x;
    double center_y;
    double mid_lon;
    double mid_lat;

    POINT project(const cheetah::Location& point) const {
        const double x = center_x + (point.longitude - mid_lon) * scale;
        const double y = center_y - (point.latitude - mid_lat) * scale;
        return {
            static_cast<LONG>(std::lround(std::clamp(x, -1e6, 1e6))),
            static_cast<LONG>(std::lround(std::clamp(y, -1e6, 1e6)))
        };
    }

    cheetah::GeoRect visible_region(const RECT& area) const {
        return {mid_lat + (center_y - area.bottom) / scale,
                mid_lat + (center_y - area.top) / scale,
                mid_lon + (area.left - center_x) / scale,
                mid_lon + (area.right - center_x) / scale};
    }
};

ScreenTransform screen_transform(const RECT& area, const MapView& view) {
    const Bounds& bounds = app.full_extent ? app.full : app.overview;
    const double lon_span = std::max(1e-9, bounds.max_lon - bounds.min_lon);
    const double lat_span = std::max(1e-9, bounds.max_lat - bounds.min_lat);
    const double width = std::max<LONG>(1, area.right - area.left - 48);
    const double height = std::max<LONG>(1, area.bottom - area.top - 48);
    const double scale = std::min(width / lon_span, height / lat_span) * view.zoom;
    const double center_x = (area.left + area.right) / 2.0 + view.pan_x;
    const double center_y = (area.top + area.bottom) / 2.0 + view.pan_y;
    const double mid_lon = (bounds.min_lon + bounds.max_lon) / 2.0;
    const double mid_lat = (bounds.min_lat + bounds.max_lat) / 2.0;
    return {scale, center_x, center_y, mid_lon, mid_lat};
}

std::optional<std::size_t> panel_at(const RECT& client, POINT point) {
    for (std::size_t i = 0; i < app.panels.size(); ++i) {
        const RECT area = panel_area(client, i, app.panels.size());
        if (PtInRect(&area, point)) return i;
    }
    return std::nullopt;
}

bool zoom_panel_at(const RECT& client, POINT point, int wheel_delta) {
    const auto index = panel_at(client, point);
    if (!index) return false;
    app.active_panel = *index;
    auto& view = app.panels[*index].view;
    const RECT area = panel_area(client, *index, app.panels.size());
    const auto previous = screen_transform(area, view);
    const double zoom = std::clamp(
        view.zoom * std::pow(1.2, wheel_delta / static_cast<double>(WHEEL_DELTA)), 0.5, 40.0);
    const double factor = zoom / view.zoom;
    view.pan_x += (point.x - previous.center_x) * (1.0 - factor);
    view.pan_y += (point.y - previous.center_y) * (1.0 - factor);
    view.zoom = zoom;
    return true;
}

void reset_views() {
    for (auto& panel : app.panels) panel.view = {};
}

void draw_basemap(HDC dc, const RECT& area, const ScreenTransform& transform) {
    const auto visible = transform.visible_region(area);
    // Light colors blend the map with the panel background beneath the trajectories.
    const COLORREF colors[] = {
        RGB(216, 235, 244), RGB(196, 222, 234), RGB(227, 230, 233),
        RGB(203, 207, 211), RGB(210, 199, 184)
    };
    const int widths[] = {1, 2, 1, 1, 2};
    std::vector<POINT> projected;
    for (std::size_t layer = 0; layer < basemap.size(); ++layer) {
        if (layer == static_cast<std::size_t>(MapLayer::Local) && transform.scale < 2100.0)
            continue;
        HPEN pen = CreatePen(PS_SOLID, widths[layer], colors[layer]);
        if (!pen) continue;
        const bool filled = layer == static_cast<std::size_t>(MapLayer::Water);
        HBRUSH brush = filled ? CreateSolidBrush(colors[layer]) : nullptr;
        HGDIOBJ previous = SelectObject(dc, pen);
        HGDIOBJ previous_brush = brush ? SelectObject(dc, brush) : nullptr;
        for (const auto& feature : basemap[layer]) {
            if (!feature.bounds.intersects(visible)) continue;
            projected.clear();
            for (const auto& point : feature.points) projected.push_back(transform.project(point));
            if (brush) Polygon(dc, projected.data(), static_cast<int>(projected.size()));
            else Polyline(dc, projected.data(), static_cast<int>(projected.size()));
        }
        if (brush) {
            SelectObject(dc, previous_brush);
            DeleteObject(brush);
        }
        SelectObject(dc, previous);
        DeleteObject(pen);
    }
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
    const auto transform = screen_transform(area, panel.view);
    if (app.show_basemap && basemap_loaded) draw_basemap(dc, area, transform);
    for (std::size_t index = 0; index < panel.selected.trajectories.size(); ++index) {
        const auto& trajectory = panel.selected.trajectories[index];
        if (trajectory.locations.empty()) continue;
        const COLORREF trajectory_color = app.color_by_representativeness &&
                                         index < panel.representative_scores.size()
            ? representative_color(panel.representative_scores[index]) : color;
        HPEN pen = CreatePen(PS_SOLID, 2, trajectory_color);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        POINT first = transform.project(trajectory.locations.front());
        if (trajectory.locations.size() == 1) {
            SetPixel(dc, first.x, first.y, trajectory_color);
        } else {
            MoveToEx(dc, first.x, first.y, nullptr);
            for (std::size_t i = 1; i < trajectory.locations.size(); ++i) {
                const POINT next = transform.project(trajectory.locations[i]);
                LineTo(dc, next.x, next.y);
            }
        }
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }
    if (app.show_basemap && basemap_loaded) {
        constexpr wchar_t credit[] = L"\u00A9 OpenStreetMap contributors (ODbL)";
        constexpr int length = static_cast<int>(sizeof(credit) / sizeof(*credit) - 1);
        SIZE size{};
        GetTextExtentPoint32W(dc, credit, length, &size);
        const LONG x = std::max(area.left + 6, area.right - size.cx - 8);
        RECT badge{x - 4, area.bottom - 21, area.right, area.bottom};
        FillRect(dc, &badge, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        SetTextColor(dc, RGB(80, 90, 100));
        TextOutW(dc, x, area.bottom - 19, credit, length);
    }
    RestoreDC(dc, saved);
}

void draw_scene(HDC dc, const RECT& client) {
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    label(dc, 15, 12, app.panels.size() == 1
        ? "CheetahTraj temporal | una ventana" :
          "CheetahTraj temporal | comparacion de ventanas");
    label(dc, 15, 36,
          app.restricted_to_region
              ? "Rueda: zoom  |  Clic y flechas: mover panel  |  R: zona consultada  |  M: mapa  |  C: color"
              : "Rueda: zoom  |  Clic y flechas: mover panel  |  R: vista general  |  F: GPS completo  |  M: mapa  |  C: color");
    for (std::size_t i = 0; i < app.panels.size(); ++i) {
        const RECT area = panel_area(client, i, app.panels.size());
        const auto& panel = app.panels[i];
        draw_panel(dc, area, panel, i == 0 ? RGB(21, 108, 170) : RGB(205, 90, 51));
        RECT panel_header{area.left, area.top - kHeaderHeight, area.right, area.top};
        const bool active = i == app.active_panel && app.panels.size() > 1;
        HBRUSH header_background = CreateSolidBrush(active ? RGB(232, 241, 250) : RGB(247, 248, 250));
        FillRect(dc, &panel_header, header_background);
        DeleteObject(header_background);
        const int saved = SaveDC(dc);
        IntersectClipRect(dc, panel_header.left, panel_header.top, panel_header.right, panel_header.bottom);
        std::ostringstream status;
        status << "W" << panel.info.id << "  " << format_utc(panel.info.start)
               << " a " << format_utc(panel.info.end) << " UTC";
        if (panel.available_segments == 0) {
            status << "  |  Sin puntos GPS en la zona";
        } else {
            status << "  |  " << panel.selected.trajectories.size() << "/"
                   << panel.available_segments << " segmentos"
                   << "  |  Q=" << std::fixed << std::setprecision(3) << panel.result.quality;
        }
        label(dc, area.left + 12, panel_header.top + 4, status.str());
        std::ostringstream view_status;
        view_status << "Zoom " << std::fixed << std::setprecision(2) << panel.view.zoom << "x";
        if (active) view_status << "  |  Panel activo";
        label(dc, area.left + 12, panel_header.top + 23, view_status.str());
        RestoreDC(dc, saved);
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
    } else if (!app.restricted_to_region) {
        label(dc, 15, client.bottom - 23,
              app.full_extent ? "Encuadre completo de los GPS (R vuelve a la vista general)" :
                                "Vista general compartida al abrir. Acerca cada panel con la rueda.");
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
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            RECT client{};
            if (ScreenToClient(window, &point) && GetClientRect(window, &client) &&
                zoom_panel_at(client, point, GET_WHEEL_DELTA_WPARAM(wparam))) {
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            RECT client{};
            GetClientRect(window, &client);
            if (const auto index = panel_at(client, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                app.active_panel = *index;
                SetFocus(window);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (app.panels.empty()) break;
            auto& view = app.panels[app.active_panel].view;
            if (wparam == 'R') { app.full_extent = false; reset_views(); }
            else if (wparam == 'M') app.show_basemap = !app.show_basemap;
            else if (wparam == 'C')
                app.color_by_representativeness = !app.color_by_representativeness;
            else if (wparam == 'F' && !app.restricted_to_region) {
                app.full_extent = !app.full_extent;
                reset_views();
            } else if (wparam == VK_LEFT) view.pan_x -= 30;
            else if (wparam == VK_RIGHT) view.pan_x += 30;
            else if (wparam == VK_UP) view.pan_y -= 30;
            else if (wparam == VK_DOWN) view.pan_y += 30;
            else break;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
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

std::optional<cheetah::GeoRect> parse_region(const std::string& line) {
    std::istringstream input(line);
    input >> std::ws;
    if (input.eof()) return std::nullopt;
    cheetah::GeoRect region;
    std::string extra;
    if (!(input >> region.min_latitude >> region.max_latitude
                >> region.min_longitude >> region.max_longitude) || (input >> extra)) {
        throw std::invalid_argument("Escribe cuatro coordenadas separadas por espacios, con punto decimal");
    }
    if (!(region.min_latitude >= -90.0 && region.max_latitude <= 90.0 &&
          region.min_longitude >= -180.0 && region.max_longitude <= 180.0 &&
          region.min_latitude < region.max_latitude && region.min_longitude < region.max_longitude)) {
        throw std::invalid_argument("Limites invalidos. Usa latMin < latMax y lonMin < lonMax");
    }
    return region;
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
        std::optional<cheetah::GeoRect> region;
        while (true) {
            std::cout << "\nZona minLat maxLat minLon maxLon (Enter = zona completa, q = salir): ";
            std::string line;
            if (!std::getline(std::cin, line) || line == "q" || line == "Q") return;
            try {
                region = parse_region(line);
                break;
            } catch (const std::invalid_argument& error) {
                std::cout << error.what() << '\n';
            }
        }
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
            app.panels = query_panels(directory, chosen, target, region);
            for (const auto& panel : app.panels) {
                std::cout << "W" << panel.info.id << ": ";
                if (panel.available_segments == 0) {
                    std::cout << "sin puntos GPS en la zona.\n";
                    continue;
                }
                std::cout << panel.selected.trajectories.size() << "/"
                          << panel.available_segments << " segmentos, Q="
                          << std::fixed << std::setprecision(6)
                          << panel.result.quality << "\n";
            }
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
        const auto relative_basemap = std::filesystem::path("data") / "porto-basemap.txt";
        auto map_path = std::filesystem::absolute(argv[0]).parent_path() / relative_basemap;
        if (!std::filesystem::exists(map_path)) map_path = relative_basemap;
        try {
            basemap = read_basemap(map_path);
            basemap_loaded = true;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
        }
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

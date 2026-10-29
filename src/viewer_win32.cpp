#ifdef _WIN32

#include "cheetah/io.hpp"
#include "cheetah/projection.hpp"
#include "cheetah/vqgs.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

enum class ViewMode { Full = 1, Vqgs = 2, Color = 3, Random = 4 };

struct AppState {
    cheetah::Dataset dataset;
    std::vector<int> selected;
    std::vector<int> representative;
    std::vector<int> random_selection;
    double rate{0.005};
    int delta{64};
    ViewMode mode{ViewMode::Vqgs};
    double min_lat{};
    double max_lat{};
    double min_lon{};
    double max_lon{};
    double full_min_lat{};
    double full_max_lat{};
    double full_min_lon{};
    double full_max_lon{};
    double robust_min_lat{};
    double robust_max_lat{};
    double robust_min_lon{};
    double robust_max_lon{};
    bool robust_extent{true};
    double zoom{1.0};
    double pan_x{};
    double pan_y{};
    std::string dataset_name;
};

AppState g_app;

COLORREF gradient_color(double value) {
    value = std::clamp(value, 0.0, 1.0);
    const int red = static_cast<int>(30 + 210 * value);
    const int green = static_cast<int>(130 - 70 * value);
    const int blue = static_cast<int>(200 - 160 * value);
    return RGB(red, green, blue);
}

void calculate_bounds() {
    std::vector<double> latitudes;
    std::vector<double> longitudes;
    for (const auto& trajectory : g_app.dataset.trajectories) {
        for (const auto& location : trajectory.locations) {
            latitudes.push_back(location.latitude);
            longitudes.push_back(location.longitude);
        }
    }
    if (latitudes.empty()) {
        throw std::runtime_error("El dataset no contiene puntos GPS");
    }
    std::sort(latitudes.begin(), latitudes.end());
    std::sort(longitudes.begin(), longitudes.end());
    g_app.full_min_lat = latitudes.front();
    g_app.full_max_lat = latitudes.back();
    g_app.full_min_lon = longitudes.front();
    g_app.full_max_lon = longitudes.back();

    // El encuadre inicial ignora el 0.5 % extremo de cada lado. Unos pocos
    // puntos GPS alejados no deben comprimir todo el núcleo de trayectorias.
    const auto percentile = [](const std::vector<double>& values, double p) {
        const std::size_t index = static_cast<std::size_t>(
            p * static_cast<double>(values.size() - 1));
        return values[index];
    };
    if (latitudes.size() >= 200) {
        g_app.robust_min_lat = percentile(latitudes, 0.005);
        g_app.robust_max_lat = percentile(latitudes, 0.995);
        g_app.robust_min_lon = percentile(longitudes, 0.005);
        g_app.robust_max_lon = percentile(longitudes, 0.995);
    } else {
        g_app.robust_min_lat = g_app.full_min_lat;
        g_app.robust_max_lat = g_app.full_max_lat;
        g_app.robust_min_lon = g_app.full_min_lon;
        g_app.robust_max_lon = g_app.full_max_lon;
    }
    g_app.min_lat = g_app.robust_min_lat;
    g_app.max_lat = g_app.robust_max_lat;
    g_app.min_lon = g_app.robust_min_lon;
    g_app.max_lon = g_app.robust_max_lon;
}

void toggle_extent() {
    g_app.robust_extent = !g_app.robust_extent;
    if (g_app.robust_extent) {
        g_app.min_lat = g_app.robust_min_lat;
        g_app.max_lat = g_app.robust_max_lat;
        g_app.min_lon = g_app.robust_min_lon;
        g_app.max_lon = g_app.robust_max_lon;
    } else {
        g_app.min_lat = g_app.full_min_lat;
        g_app.max_lat = g_app.full_max_lat;
        g_app.min_lon = g_app.full_min_lon;
        g_app.max_lon = g_app.full_max_lon;
    }
    g_app.zoom = 1.0;
    g_app.pan_x = 0.0;
    g_app.pan_y = 0.0;
}

void compute_selection() {
    cheetah::ProjectionOptions projection_options;
    projection_options.zoom = 20;
    projection_options.center_latitude = (g_app.min_lat + g_app.max_lat) / 2.0;
    projection_options.center_longitude = (g_app.min_lon + g_app.max_lon) / 2.0;
    cheetah::rasterize_dataset(
        g_app.dataset, cheetah::WebMercatorProjection(projection_options));
    cheetah::VqgsOptions options;
    options.delta = g_app.delta;
    options.sampling_rate = g_app.rate;
    const cheetah::VqgsEngine engine(g_app.dataset);
    const auto result = engine.select(options);
    g_app.selected = result.trajectory_ids;
    g_app.representative = engine.representative_scores(result, g_app.delta);

    g_app.random_selection.resize(g_app.dataset.trajectories.size());
    std::iota(g_app.random_selection.begin(), g_app.random_selection.end(), 0);
    std::mt19937 generator(42);
    std::shuffle(g_app.random_selection.begin(), g_app.random_selection.end(), generator);
    g_app.random_selection.resize(g_app.selected.size());
}

void refresh_selection(HWND window) {
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    compute_selection();
    SetCursor(LoadCursor(nullptr, IDC_ARROW));
    InvalidateRect(window, nullptr, FALSE);
}

POINT to_screen(const cheetah::Location& location, const RECT& area) {
    constexpr double margin = 32.0;
    const double width = std::max(1.0, static_cast<double>(area.right - area.left) - 2 * margin);
    const double height = std::max(1.0, static_cast<double>(area.bottom - area.top) - 2 * margin - 42.0);
    const double lon_span = std::max(1e-12, g_app.max_lon - g_app.min_lon);
    const double lat_span = std::max(1e-12, g_app.max_lat - g_app.min_lat);
    const double x0 = (location.longitude - g_app.min_lon) / lon_span;
    const double y0 = (g_app.max_lat - location.latitude) / lat_span;
    const double base_x = margin + x0 * width;
    const double base_y = margin + 42.0 + y0 * height;
    const double center_x = (area.right - area.left) / 2.0;
    const double center_y = (area.bottom - area.top + 42.0) / 2.0;
    return {
        static_cast<LONG>((base_x - center_x) * g_app.zoom + center_x + g_app.pan_x),
        static_cast<LONG>((base_y - center_y) * g_app.zoom + center_y + g_app.pan_y)
    };
}

void draw_trajectory(HDC dc,
                     const cheetah::Trajectory& trajectory,
                     const RECT& area,
                     COLORREF color,
                     int width) {
    if (trajectory.locations.size() < 2) {
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    POINT first = to_screen(trajectory.locations.front(), area);
    MoveToEx(dc, first.x, first.y, nullptr);
    for (std::size_t index = 1; index < trajectory.locations.size(); ++index) {
        POINT point = to_screen(trajectory.locations[index], area);
        LineTo(dc, point.x, point.y);
    }
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}

std::string mode_name() {
    switch (g_app.mode) {
        case ViewMode::Full: return "Full";
        case ViewMode::Vqgs: return "VQGS+";
        case ViewMode::Color: return "VQGS+ color";
        case ViewMode::Random: return "Random";
    }
    return "";
}

void draw_scene(HDC dc, const RECT& area) {
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(dc, TRANSPARENT);

    if (g_app.mode == ViewMode::Full) {
        for (const auto& trajectory : g_app.dataset.trajectories) {
            draw_trajectory(dc, trajectory, area, RGB(110, 110, 110), 1);
        }
    } else {
        const std::vector<int>& ids =
            g_app.mode == ViewMode::Random ? g_app.random_selection : g_app.selected;
        int max_rep = 1;
        if (!g_app.representative.empty()) {
            max_rep = std::max(1, *std::max_element(
                g_app.representative.begin(), g_app.representative.end()));
        }
        for (std::size_t index = 0; index < ids.size(); ++index) {
            COLORREF color = g_app.mode == ViewMode::Random
                ? RGB(220, 120, 35)
                : RGB(20, 105, 175);
            if (g_app.mode == ViewMode::Color && index < g_app.representative.size()) {
                color = gradient_color(
                    std::log1p(static_cast<double>(g_app.representative[index])) /
                    std::log1p(static_cast<double>(max_rep)));
            }
            draw_trajectory(dc,
                            g_app.dataset.trajectories[static_cast<std::size_t>(ids[index])],
                            area,
                            color,
                            2);
        }
    }

    RECT header{0, 0, area.right, 42};
    FillRect(dc, &header, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    std::ostringstream status;
    status << "CheetahTraj C++ | " << mode_name()
           << " | n=" << g_app.dataset.trajectories.size()
           << " | muestra=" << g_app.selected.size()
           << " | rate=" << std::fixed << std::setprecision(4) << g_app.rate
           << " | delta=" << g_app.delta
           << " | encuadre=" << (g_app.robust_extent ? "principal" : "completo");
    const std::string text = status.str();
    TextOutA(dc, 12, 7, text.c_str(), static_cast<int>(text.size()));
    const std::string help = "1-4 Vistas  A/D Delta  W/S Tasa  F Encuadre  R Reset  P Captura  Rueda Zoom";
    TextOutA(dc, 12, 23, help.c_str(), static_cast<int>(help.size()));
}

bool save_client_bmp(HWND window, const std::filesystem::path& path) {
    RECT rect{};
    GetClientRect(window, &rect);
    const int width = rect.right;
    const int height = rect.bottom;
    HDC window_dc = GetDC(window);
    HDC memory_dc = CreateCompatibleDC(window_dc);
    HBITMAP bitmap = CreateCompatibleBitmap(window_dc, width, height);
    HGDIOBJ old = SelectObject(memory_dc, bitmap);
    draw_scene(memory_dc, rect);

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = width;
    header.biHeight = height;
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    const DWORD row_size = static_cast<DWORD>((width * 3 + 3) & ~3);
    const DWORD image_size = row_size * static_cast<DWORD>(height);
    std::vector<unsigned char> pixels(image_size);
    BITMAPINFO info{};
    info.bmiHeader = header;
    GetDIBits(memory_dc, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);

    BITMAPFILEHEADER file_header{};
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + image_size;
    HANDLE file = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool success = file != INVALID_HANDLE_VALUE;
    if (success) {
        DWORD written{};
        success = WriteFile(file, &file_header, sizeof(file_header), &written, nullptr) &&
                  WriteFile(file, &header, sizeof(header), &written, nullptr) &&
                  WriteFile(file, pixels.data(), image_size, &written, nullptr);
        CloseHandle(file);
    }
    SelectObject(memory_dc, old);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(window, window_dc);
    return success;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT area{};
            GetClientRect(window, &area);
            HDC buffer_dc = CreateCompatibleDC(dc);
            HBITMAP buffer = CreateCompatibleBitmap(dc, area.right, area.bottom);
            HGDIOBJ old = SelectObject(buffer_dc, buffer);
            draw_scene(buffer_dc, area);
            BitBlt(dc, 0, 0, area.right, area.bottom, buffer_dc, 0, 0, SRCCOPY);
            SelectObject(buffer_dc, old);
            DeleteObject(buffer);
            DeleteDC(buffer_dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            g_app.zoom = std::clamp(
                g_app.zoom * (delta > 0 ? 1.2 : 1.0 / 1.2), 0.5, 20.0);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            switch (wparam) {
                case '1': g_app.mode = ViewMode::Full; break;
                case '2': g_app.mode = ViewMode::Vqgs; break;
                case '3': g_app.mode = ViewMode::Color; break;
                case '4': g_app.mode = ViewMode::Random; break;
                case 'A':
                    g_app.delta = std::max(0, g_app.delta - 4);
                    refresh_selection(window);
                    return 0;
                case 'D':
                    g_app.delta += 4;
                    refresh_selection(window);
                    return 0;
                case 'W':
                    g_app.rate = std::max(0.0001, g_app.rate / 2.0);
                    refresh_selection(window);
                    return 0;
                case 'S':
                    g_app.rate = std::min(1.0, g_app.rate * 2.0);
                    refresh_selection(window);
                    return 0;
                case 'F':
                    toggle_extent();
                    break;
                case 'R':
                    g_app.zoom = 1.0;
                    g_app.pan_x = 0.0;
                    g_app.pan_y = 0.0;
                    break;
                case 'P':
                    save_client_bmp(window, "cheetahtraj-screenshot.bmp");
                    MessageBoxA(window,
                                "Captura guardada como cheetahtraj-screenshot.bmp",
                                "CheetahTraj C++",
                                MB_OK | MB_ICONINFORMATION);
                    break;
                case VK_LEFT: g_app.pan_x += 30.0; break;
                case VK_RIGHT: g_app.pan_x -= 30.0; break;
                case VK_UP: g_app.pan_y += 30.0; break;
                case VK_DOWN: g_app.pan_y -= 30.0; break;
                case VK_ESCAPE: DestroyWindow(window); return 0;
                default: return DefWindowProc(window, message, wparam, lparam);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProc(window, message, wparam, lparam);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path dataset = "data/porto_5k/porto_5k.txt";
        std::size_t limit = 0;
        if (argc > 1) {
            dataset = argv[1];
        }
        if (argc > 2) {
            g_app.rate = std::stod(argv[2]);
        }
        if (argc > 3) {
            g_app.delta = std::stoi(argv[3]);
        }
        if (argc > 4) {
            limit = static_cast<std::size_t>(std::stoull(argv[4]));
        }
        std::cout << "Cargando " << dataset.string() << "...\n";
        g_app.dataset = cheetah::load_dataset(dataset, {limit, false});
        g_app.dataset_name = dataset.filename().string();
        calculate_bounds();
        compute_selection();

        HINSTANCE instance = GetModuleHandle(nullptr);
        WNDCLASSA window_class{};
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = "CheetahTrajCppViewer";
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        if (!RegisterClassA(&window_class)) {
            throw std::runtime_error("No se pudo registrar la ventana");
        }

        HWND window = CreateWindowExA(
            0,
            window_class.lpszClassName,
            "CheetahTraj C++ Viewer",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            1200,
            800,
            nullptr,
            nullptr,
            instance,
            nullptr);
        if (!window) {
            throw std::runtime_error("No se pudo crear la ventana");
        }
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);

        MSG message{};
        while (GetMessage(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        return static_cast<int>(message.wParam);
    } catch (const std::exception& error) {
        MessageBoxA(nullptr, error.what(), "CheetahTraj C++ - Error", MB_OK | MB_ICONERROR);
        return 1;
    }
}

#else

int main() { return 1; }

#endif

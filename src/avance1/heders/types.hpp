#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace cheetah {

using Timestamp = std::int64_t;
constexpr Timestamp kNoTimestamp = std::numeric_limits<Timestamp>::min();

struct Location {
    double longitude{};
    double latitude{};
    Timestamp timestamp{kNoTimestamp};

    bool has_timestamp() const noexcept { return timestamp != kNoTimestamp; }
};

struct Pixel {
    int x{};
    int y{};

    bool operator==(const Pixel& other) const noexcept {
        return x == other.x && y == other.y;
    }

    bool operator<(const Pixel& other) const noexcept {
        return x < other.x || (x == other.x && y < other.y);
    }
};

struct PixelHash {
    std::size_t operator()(const Pixel& pixel) const noexcept {
        const std::uint64_t ux = static_cast<std::uint32_t>(pixel.x);
        const std::uint64_t uy = static_cast<std::uint32_t>(pixel.y);
        std::uint64_t value = ux * 0x9E3779B185EBCA87ULL;
        value ^= uy + 0x9E3779B97F4A7C15ULL + (value << 6U) + (value >> 2U);
        return static_cast<std::size_t>(value);
    }
};

struct Trajectory {
    int id{};
    // Stable identifier in the source dataset. Unlike id, this value is kept
    // when a trajectory is sliced into different temporal windows.
    std::int64_t source_id{-1};
    std::vector<Location> locations;
    std::vector<Pixel> pixels;
    int original_score{};
};

struct Dataset {
    std::vector<Trajectory> trajectories;
    std::string source_path;
};

}  // namespace cheetah

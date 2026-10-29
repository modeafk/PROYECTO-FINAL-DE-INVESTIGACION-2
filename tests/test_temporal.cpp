#include "cheetah/io.hpp"
#include "cheetah/projection.hpp"
#include "cheetah/temporal.hpp"
#include "cheetah/temporal_invquad.hpp"
#include "cheetah/vqgs.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <unordered_set>

namespace {

cheetah::Trajectory make_temporal_trajectory(std::int64_t source_id,
                                             double longitude,
                                             cheetah::Timestamp offset) {
    cheetah::Trajectory trajectory;
    trajectory.source_id = source_id;
    trajectory.locations = {
        {longitude, 41.14, offset + 0},
        {longitude + 0.001, 41.141, offset + 5},
        {longitude + 0.002, 41.142, offset + 10},
        {longitude + 0.003, 41.143, offset + 15}
    };
    return trajectory;
}

void test_half_open_windows_and_source_ids() {
    cheetah::Dataset dataset;
    dataset.trajectories = {
        make_temporal_trajectory(101, -8.64, 0),
        make_temporal_trajectory(202, -8.65, 0)
    };
    for (std::size_t index = 0; index < dataset.trajectories.size(); ++index) {
        dataset.trajectories[index].id = static_cast<int>(index);
    }

    const auto windows = cheetah::generate_time_windows(0, 20, 10, 10);
    assert(windows.size() == 2);
    const auto first = cheetah::filter_by_time_window(dataset, windows[0]);
    const auto second = cheetah::filter_by_time_window(dataset, windows[1]);
    assert(first.trajectories.size() == 2);
    assert(second.trajectories.size() == 2);
    assert(first.trajectories[0].locations.size() == 2);
    assert(second.trajectories[0].locations.size() == 2);
    assert(first.trajectories[0].locations.back().timestamp == 5);
    assert(second.trajectories[0].locations.front().timestamp == 10);
    assert(first.trajectories[0].source_id == 101);
    assert(second.trajectories[0].source_id == 101);
}

void test_temporal_window_runs_original_vqgs() {
    cheetah::Dataset dataset;
    dataset.trajectories = {
        make_temporal_trajectory(101, -8.64, 0),
        make_temporal_trajectory(202, -8.65, 0)
    };
    for (std::size_t index = 0; index < dataset.trajectories.size(); ++index) {
        dataset.trajectories[index].id = static_cast<int>(index);
    }
    auto window = cheetah::filter_by_time_window(dataset, {0, 10});
    cheetah::ProjectionOptions projection_options;
    projection_options.zoom = 15;
    projection_options.center_latitude = 41.14;
    projection_options.center_longitude = -8.645;
    cheetah::rasterize_dataset(
        window, cheetah::WebMercatorProjection(projection_options));
    cheetah::VqgsOptions options;
    options.delta = 0;
    options.sample_count = 1;
    const cheetah::VqgsEngine engine(window);
    const auto result = engine.select(options);
    assert(result.trajectory_ids.size() == 1);
    assert(engine.quality(result) > 0.0);
    assert(engine.quality(result) <= 1.0);
}

void test_time_bounds_are_exclusive() {
    cheetah::Dataset dataset;
    dataset.trajectories = {make_temporal_trajectory(101, -8.64, 100)};
    const auto bounds = cheetah::dataset_time_bounds(dataset);
    assert(bounds.has_value());
    assert(bounds->first == 100);
    assert(bounds->second == 116);
}

void test_quality_target_mode() {
    cheetah::Dataset dataset;
    cheetah::Trajectory first;
    first.id = 0;
    first.pixels = {{0, 0}, {1, 0}};
    first.original_score = 2;
    cheetah::Trajectory second;
    second.id = 1;
    second.pixels = {{10, 0}, {11, 0}};
    second.original_score = 2;
    dataset.trajectories = {first, second};

    cheetah::VqgsOptions options;
    options.delta = 0;
    options.sample_count = 1;  // Ignored in quality-target mode.
    options.quality_target = 0.8;
    const cheetah::VqgsEngine engine(dataset);
    const auto result = engine.select(options);
    assert(result.trajectory_ids.size() == 2);
    assert(engine.quality(result) >= 0.8);
}

void test_sampling_rate_uses_ceiling() {
    cheetah::Dataset dataset;
    for (int id = 0; id < 3; ++id) {
        cheetah::Trajectory trajectory;
        trajectory.id = id;
        trajectory.source_id = id;
        trajectory.pixels.push_back({id, id});
        trajectory.original_score = 1;
        dataset.trajectories.push_back(std::move(trajectory));
    }

    cheetah::VqgsOptions options;
    options.delta = 0;
    options.sample_count = 0;
    options.sampling_rate = 0.5;

    const cheetah::VqgsEngine engine(dataset);
    const auto result = engine.select(options);
    assert(result.trajectory_ids.size() == 2);
}

void test_porto_loader_reconstructs_point_times() {
    const auto path = std::filesystem::temp_directory_path() /
                      "cheetahtraj_porto_loader_test.csv";
    {
        std::ofstream output(path);
        output
            << "\"TRIP_ID\",\"CALL_TYPE\",\"ORIGIN_CALL\",\"ORIGIN_STAND\","
               "\"TAXI_ID\",\"TIMESTAMP\",\"DAY_TYPE\",\"MISSING_DATA\","
               "\"POLYLINE\"\n"
            << "\"9001\",\"C\",\"\",\"\",\"1\",\"1000\",\"A\","
               "\"False\",\"[[-8.64,41.14],[-8.63,41.15],[-8.62,41.16]]\"\n"
            << "\"9002\",\"C\",\"\",\"\",\"1\",\"1000\",\"A\","
               "\"True\",\"[[-8.64,41.14],[-8.63,41.15]]\"\n";
    }
    const auto dataset = cheetah::load_porto_train_dataset(path);
    std::filesystem::remove(path);
    assert(dataset.trajectories.size() == 1);
    assert(dataset.trajectories[0].source_id == 9001);
    assert(dataset.trajectories[0].locations.size() == 3);
    assert(dataset.trajectories[0].locations[0].timestamp == 1000);
    assert(dataset.trajectories[0].locations[1].timestamp == 1015);
    assert(dataset.trajectories[0].locations[2].timestamp == 1030);
}

void test_spatiotemporal_index_persists_and_queries() {
    cheetah::Dataset dataset;
    dataset.trajectories = {
        make_temporal_trajectory(101, -8.64, 0),
        make_temporal_trajectory(202, -8.65, 0),
        make_temporal_trajectory(303, -8.64, 20)
    };
    for (std::size_t i = 0; i < dataset.trajectories.size(); ++i) {
        dataset.trajectories[i].id = static_cast<int>(i);
    }
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
        ("cheetah_temporal_index_" + std::to_string(unique));
    cheetah::TemporalIndexOptions options;
    options.window_size = 20;
    options.window_step = 20;
    options.height = 2;
    options.delta = 0;
    options.projection.zoom = 15;
    options.projection.center_latitude = 41.14;
    options.projection.center_longitude = -8.645;
    const auto report = cheetah::TemporalInvQuadIndex::build(
        dataset, directory, options);
    assert(report.windows == 2);
    assert(report.nonempty_windows == 2);
    const cheetah::GeoRect region{41.13, 41.16, -8.66, -8.63};
    const auto first = cheetah::TemporalInvQuadIndex::query(
        directory, 0, region, 0.9);
    const auto second = cheetah::TemporalInvQuadIndex::query(
        directory, 1, region, 0.9);
    assert(first.target_met && second.target_met);
    assert(first.quality >= 0.9 && second.quality >= 0.9);
    assert(second.source_ids.size() == 1);
    assert(second.source_ids[0] == 303);
    for (auto id : first.source_ids) assert(id != 303);
    // The color score is computed after selection, using all original
    // trajectories in the same temporal window as the reference set.
    for (int window_id = 0; window_id < 2; ++window_id) {
        auto window = cheetah::load_temporal_dataset(
            directory / ("window_" + std::to_string(window_id) + ".csv"));
        cheetah::rasterize_dataset(
            window, cheetah::WebMercatorProjection(options.projection));
        const auto& query_result = window_id == 0 ? first : second;
        const std::unordered_set<std::int64_t> selected_ids(
            query_result.source_ids.begin(), query_result.source_ids.end());
        cheetah::VqgsResult sample;
        for (const auto& trajectory : window.trajectories)
            if (selected_ids.count(trajectory.source_id))
                sample.trajectory_ids.push_back(trajectory.id);
        assert(sample.trajectory_ids.size() == selected_ids.size());
        const auto scores = cheetah::VqgsEngine(window).representative_scores(
            sample, options.delta);
        assert(scores.size() == sample.trajectory_ids.size());
        assert(std::accumulate(scores.begin(), scores.end(), 0) ==
               static_cast<int>(window.trajectories.size()));
    }
    std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
    test_half_open_windows_and_source_ids();
    test_temporal_window_runs_original_vqgs();
    test_time_bounds_are_exclusive();
    test_quality_target_mode();
    test_sampling_rate_uses_ceiling();
    test_porto_loader_reconstructs_point_times();
    test_spatiotemporal_index_persists_and_queries();
    std::cout << "Todas las pruebas temporales pasaron.\n";
    return 0;
}

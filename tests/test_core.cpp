#include "cheetah/projection.hpp"
#include "cheetah/invquad.hpp"
#include "cheetah/quadtree.hpp"
#include "cheetah/vqgs.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

cheetah::Trajectory make_trajectory(int id,
                                    std::initializer_list<cheetah::Pixel> pixels) {
    cheetah::Trajectory trajectory;
    trajectory.id = id;
    trajectory.pixels = pixels;
    trajectory.original_score = static_cast<int>(trajectory.pixels.size());
    return trajectory;
}

void test_vqgs_prefers_new_coverage() {
    cheetah::Dataset dataset;
    dataset.trajectories = {
        make_trajectory(0, {{0, 0}, {1, 0}, {2, 0}}),
        make_trajectory(1, {{0, 0}, {1, 0}}),
        make_trajectory(2, {{10, 0}, {11, 0}})
    };
    cheetah::VqgsEngine engine(dataset);
    cheetah::VqgsOptions options;
    options.delta = 0;
    options.sample_count = 2;
    const auto result = engine.select(options);
    assert(result.trajectory_ids.size() == 2);
    assert(result.trajectory_ids[0] == 0);
    assert(result.trajectory_ids[1] == 2);
    assert(std::abs(engine.quality(result) - 1.0) < 1e-12);
}

void test_delta_suppresses_nearby_pixels() {
    cheetah::Dataset dataset;
    dataset.trajectories = {
        make_trajectory(0, {{0, 0}, {1, 0}, {2, 0}}),
        make_trajectory(1, {{0, 1}, {1, 1}, {2, 1}}),
        make_trajectory(2, {{10, 0}, {11, 0}})
    };
    cheetah::VqgsEngine engine(dataset);
    cheetah::VqgsOptions options;
    options.delta = 1;
    options.sample_count = 2;
    const auto result = engine.select(options);
    assert(result.trajectory_ids[0] == 0);
    assert(result.trajectory_ids[1] == 2);
}

void test_projection_is_stable() {
    cheetah::ProjectionOptions options;
    options.zoom = 20;
    options.center_latitude = 41.142063;
    options.center_longitude = -8.639684;
    options.viewport_width = 1200;
    options.viewport_height = 800;
    cheetah::WebMercatorProjection projection(options);
    const auto center = projection.project({-8.639684, 41.142063});
    assert(center.x == 600);
    assert(center.y == 400);
}

void test_quality_at_projection() {
    cheetah::Dataset dataset;
    cheetah::Trajectory first;
    first.id = 0;
    first.locations = {{-8.6400, 41.1400}, {-8.6399, 41.1401}};
    cheetah::Trajectory second;
    second.id = 1;
    second.locations = {{-8.6300, 41.1500}};
    dataset.trajectories = {first, second};

    cheetah::ProjectionOptions options;
    options.zoom = 15;
    options.center_latitude = 41.145;
    options.center_longitude = -8.635;
    cheetah::WebMercatorProjection projection(options);
    const double partial = cheetah::quality_at_projection(dataset, {0}, projection, 0);
    const double full = cheetah::quality_at_projection(dataset, {0, 1}, projection, 0);
    assert(partial > 0.0 && partial < 1.0);
    assert(std::abs(full - 1.0) < 1e-12);
}

void test_quadtree_query() {
    cheetah::Dataset dataset;
    cheetah::Trajectory first;
    first.id = 0;
    first.locations = {{-8.64, 41.14}, {-8.63, 41.15}};
    cheetah::Trajectory second;
    second.id = 1;
    second.locations = {{114.16, 22.56}};
    dataset.trajectories = {first, second};

    cheetah::QuadTreeIndex index(dataset, 3);
    const auto result = index.query({41.0, 42.0, -9.0, -8.0});
    assert(result.size() == 1);
    assert(result[0] == 0);
}

void test_invquad_query() {
    cheetah::Dataset dataset;
    for (int id = 0; id < 3; ++id) {
        cheetah::Trajectory trajectory;
        trajectory.id = id;
        trajectory.locations = {
            {-8.64 + id * 0.01, 41.14 + id * 0.01},
            {-8.639 + id * 0.01, 41.141 + id * 0.01}
        };
        dataset.trajectories.push_back(trajectory);
    }
    cheetah::ProjectionOptions options;
    options.zoom = 14;
    options.center_latitude = 41.15;
    options.center_longitude = -8.63;
    cheetah::WebMercatorProjection projection(options);
    cheetah::rasterize_dataset(dataset, projection);
    cheetah::InvQuadIndex index(dataset, projection, 2, 0);
    const auto result = index.query(index.bounds(), 0.5);
    assert(!result.empty());
    assert(result.size() <= dataset.trajectories.size());
}

}  // namespace

int main() {
    test_vqgs_prefers_new_coverage();
    test_delta_suppresses_nearby_pixels();
    test_projection_is_stable();
    test_quality_at_projection();
    test_quadtree_query();
    test_invquad_query();
    std::cout << "Todas las pruebas pasaron.\n";
    return 0;
}

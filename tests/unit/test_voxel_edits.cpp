// Voxel edits that remove nothing must change nothing: no grid growth, no empty cells, no chunk
// turned into a grid. And a chunk carved down to nothing is known to have no geometry, so the GPU
// upload can draw it as empty (a bomb crater in examples/16-sandbox once drew other parts of the
// arena where cells had been emptied or created empty).

#include "doctest/doctest.h"

#include <vector>

#include "utils/editing.h"
#include "utils/scene_query.h"

namespace {
    using projv::ComponentHandle;
    using projv::core::ivec3;
    namespace utils = projv::utils;

    std::vector<projv::PendingVoxelOp> box(ivec3 low, ivec3 high, bool add) {
        std::vector<projv::PendingVoxelOp> ops;
        for (int z = low.z; z <= high.z; z++)
            for (int y = low.y; y <= high.y; y++)
                for (int x = low.x; x <= high.x; x++) ops.push_back({add, ivec3(x, y, z), 0x3FFFFFFFu});
        return ops;
    }

    size_t liveCells(const projv::Scene& scene, ComponentHandle h) {
        const projv::SceneGrid& g = scene.grids[size_t(scene.components[h].gridIndex)];
        size_t n = 0;
        for (int32_t c : g.cellToChunk) n += c >= 0;
        return n;
    }
}

TEST_CASE("removing voxels where a grid has no cells neither grows it nor makes cells") {
    projv::Scene scene;
    ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "g", projv::INVALID_COMPONENT_HANDLE, 16, 0.5f);
    utils::queueVoxelAdd(scene, h, box({0, 0, 0}, {39, 1, 39}, true));   // a 3x1x3-cell floor
    utils::updateScene(scene);
    REQUIRE(scene.components[h].kind == projv::ComponentKind::Grid);
    const projv::SceneGrid& grid = scene.grids[size_t(scene.components[h].gridIndex)];
    ivec3 dims = grid.dims, origin = grid.originCellCoord;
    size_t cells = liveCells(scene, h), chunks = scene.chunks.size();

    // A crater that reaches below, above and beside the floor: most of it is empty space.
    utils::queueVoxelRemove(scene, h, box({-20, -20, -20}, {60, 40, 60}, false));
    utils::updateScene(scene);
    CHECK(grid.dims == dims);
    CHECK(grid.originCellCoord == origin);
    CHECK(liveCells(scene, h) == cells);
    CHECK(scene.chunks.size() == chunks);
    // ...and it did remove what was there: every cell is carved hollow, and known to be.
    for (int32_t c : grid.cellToChunk)
        if (c >= 0) CHECK_FALSE(utils::chunkHasGeometry(scene, projv::ChunkHandle(c)));
}

TEST_CASE("removing voxels outside a chunk's cube does not turn it into a grid") {
    projv::Scene scene;
    ComponentHandle h = utils::addComponent(scene, projv::ComponentKind::Chunk, "c", projv::INVALID_COMPONENT_HANDLE, 16, 0.5f);
    utils::queueVoxelAdd(scene, h, box({0, 0, 0}, {3, 3, 3}, true));
    utils::updateScene(scene);
    utils::queueVoxelRemove(scene, h, box({-4, -4, -4}, {20, 1, 20}, false));
    utils::updateScene(scene);
    CHECK(scene.components[h].kind == projv::ComponentKind::Chunk);
    projv::ChunkHandle chunk = scene.components[h].chunkHandle;
    CHECK(utils::chunkHasGeometry(scene, chunk));   // rows 2 and 3 are left

    // Adding outside the cube still grows it into a grid: placing voxels is what grows things.
    utils::queueVoxelAdd(scene, h, box({20, 0, 0}, {20, 0, 0}, true));
    utils::updateScene(scene);
    CHECK(scene.components[h].kind == projv::ComponentKind::Grid);
}

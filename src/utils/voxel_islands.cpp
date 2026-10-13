#include "utils/voxel_islands.h"

#include <algorithm>
#include <string>

#include "core/log.h"
#include "utils/collision_geometry.h"
#include "utils/editing.h"
#include "utils/scene_query.h"

namespace projv::utils {
    std::vector<std::vector<core::ivec3>> voxelIslands(const GeometryBlob& blob, uint32_t resolution, uint64_t maxCells) {
        std::vector<std::vector<core::ivec3>> islands;
        VoxelOccupancy o;
        if (decodeOccupancy(blob, resolution, maxCells, o) != DecodeResult::Ok) return islands;
        const core::ivec3 d = o.dims;
        constexpr uint8_t SEEN = 2;
        static const core::ivec3 faces[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        std::vector<core::ivec3> stack;
        for (int z = 0; z < d.z; z++)
            for (int y = 0; y < d.y; y++)
                for (int x = 0; x < d.x; x++) {
                    if (o.cells[o.index({x, y, z})] != 1) continue;
                    std::vector<core::ivec3> island;
                    o.cells[o.index({x, y, z})] |= SEEN;
                    stack.push_back({x, y, z});
                    while (!stack.empty()) {
                        core::ivec3 p = stack.back();
                        stack.pop_back();
                        island.push_back(p + o.origin);
                        for (core::ivec3 f : faces) {
                            core::ivec3 n = p + f;
                            if (glm::any(glm::lessThan(n, core::ivec3(0))) || glm::any(glm::greaterThanEqual(n, d))) continue;
                            uint8_t& cell = o.cells[o.index(n)];
                            if (cell != 1) continue;
                            cell |= SEEN;
                            stack.push_back(n);
                        }
                    }
                    islands.push_back(std::move(island));
                }
        return islands;
    }

    IslandSplit splitVoxelIslands(Scene& scene, ComponentHandle component, bool piecesAsRoots) {
        IslandSplit split;
        if (!isComponentAlive(scene, component)) return split;
        const ComponentRecord& record = scene.components[component];
        if (record.kind != ComponentKind::Chunk || record.chunkHandle >= scene.chunks.size()) return split;
        const Chunk& chunk = scene.chunks[record.chunkHandle];
        if (chunk.geometryPoolIndex < 0 || size_t(chunk.geometryPoolIndex) >= scene.geometryPool.size()) return split;
        const uint32_t resolution = chunk.header.resolution;
        std::vector<std::vector<core::ivec3>> islands =
            voxelIslands(scene.geometryPool[size_t(chunk.geometryPoolIndex)], resolution);
        if (islands.size() < 2) return split;

        size_t keep = 0;
        for (size_t i = 1; i < islands.size(); i++)
            if (islands[i].size() > islands[keep].size()) keep = i;
        split.voxels.push_back(islands[keep].size());

        // Each piece starts as a copy of the whole component -- palette, transform, attachments, its
        // geometry shared until edited -- and has every voxel not in its island carved away. The
        // original loses the pieces' voxels.
        const std::string name = record.name;
        const ComponentHandle parent = piecesAsRoots ? INVALID_COMPONENT_HANDLE : record.parent;
        // As a root, a piece's local transform is its world one: the original's place in the world.
        const core::vec3 worldPosition = getComponentWorldPosition(scene, component);
        const core::quat worldRotation = getComponentWorldRotation(scene, component);
        const float worldScale = chunk.nativeScale > 0.0f ? chunk.header.scale / chunk.nativeScale : 1.0f;
        std::vector<PendingVoxelOp> removeFromOriginal;
        for (size_t i = 0; i < islands.size(); i++) {
            if (i == keep) continue;
            ComponentHandle piece = duplicateComponent(scene, component, parent);
            if (piece == INVALID_COMPONENT_HANDLE) {
                core::warn("islands: could not copy '{}' for a piece; that island stays attached", name);
                continue;
            }
            scene.components[piece].name = name + " piece " + std::to_string(split.pieces.size() + 1);
            if (piecesAsRoots) setComponentTransform(scene, piece, worldPosition, worldRotation, worldScale);
            std::vector<PendingVoxelOp> carve;
            for (size_t j = 0; j < islands.size(); j++) {
                if (j == i) continue;
                for (core::ivec3 v : islands[j]) carve.push_back({false, v, 0});
            }
            queueVoxelRemove(scene, piece, carve);
            for (core::ivec3 v : islands[i]) removeFromOriginal.push_back({false, v, 0});
            split.pieces.push_back(piece);
            split.voxels.push_back(islands[i].size());
        }
        queueVoxelRemove(scene, component, removeFromOriginal);
        updateScene(scene);
        return split;
    }
}

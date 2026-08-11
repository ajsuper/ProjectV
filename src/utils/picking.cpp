#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "utils/picking.h"
#include "utils/voxel_math.h"
#include "core/log.h"

namespace projv::utils {

    namespace {
        // Rank of a child within a tree64 node's 64-bit occupancy mask: how many set children come
        // before it. mask1 holds children 0..31 with child 0 in the most significant bit, which is
        // the layout the shader counts against (see calculateSiblingsBeforeThisZOrder) and the
        // serializer writes (mask1 = high half of the 64-bit row).
        uint64_t nodeOccupancy(uint32_t mask1, uint32_t mask2) {
            return (uint64_t(mask1) << 32) | uint64_t(mask2);
        }

        bool childIsPresent(uint64_t occupancy, uint32_t childPos) {
            return (occupancy >> (63u - childPos)) & 1ull;
        }

        uint32_t siblingsBefore(uint64_t occupancy, uint32_t childPos) {
            if (childPos == 0) return 0;
            // The `childPos` most significant bits are the children ahead of this one. Shifting a
            // 64-bit value by 64 is undefined, which is exactly the childPos == 0 case above.
            uint64_t beforeMask = ~0ull << (64u - childPos);
            return uint32_t(__builtin_popcountll(occupancy & beforeMask));
        }

        // Slab test against an axis-aligned box, returning the entry and exit distances along the
        // ray. Misses are reported as exit < entry.
        void intersectBox(core::vec3 rayOrigin, core::vec3 inverseDirection,
                          core::vec3 boxMin, core::vec3 boxMax,
                          float& entryDistance, float& exitDistance) {
            core::vec3 t0 = (boxMin - rayOrigin) * inverseDirection;
            core::vec3 t1 = (boxMax - rayOrigin) * inverseDirection;
            core::vec3 tNear = core::min(t0, t1);
            core::vec3 tFar = core::max(t0, t1);
            entryDistance = std::max(std::max(tNear.x, tNear.y), tNear.z);
            exitDistance = std::min(std::min(tFar.x, tFar.y), tFar.z);
        }

        // A chunk that the ray's world bounding box test let through, with where it entered.
        struct ChunkCandidate {
            ChunkHandle handle;
            float entryDistance;
        };

        // Which chunks are actually on screen, which is NOT the same question as which chunks are
        // alive. The renderer reaches a chunk through exactly two routes -- the loose list it
        // iterates, and a grid's cell map -- so anything in neither is invisible however alive it is.
        // This mirrors computeLooseList in gpu_interface.cpp, legacy fallback included, so the set a
        // ray tests and the set the viewport draws are the same set by construction.
        //
        // The distinction is load-bearing, not defensive. An editor hides geometry by un-listing it
        // (see setComponentRendered in the SceneEditor example: a boolean part must stop being drawn
        // while the resolved result stands in for it, yet stay fully readable, so clearing `alive`
        // is not an option). A pick that walked every live chunk would stop on that hidden part --
        // reporting a hit on a surface the user cannot see, in front of the one they aimed at.
        std::vector<bool> visibleChunks(const Scene& scene) {
            std::vector<bool> visible(scene.chunks.size(), false);

            // A scene with neither route populated is a legacy one that predates both; the renderer
            // treats every live chunk as loose, so a ray must too.
            if (scene.grids.empty() && scene.looseChunks.empty()) {
                for (size_t handle = 0; handle < scene.chunks.size(); handle++) {
                    visible[handle] = scene.chunks[handle].alive;
                }
                return visible;
            }

            for (ChunkHandle handle : scene.looseChunks) {
                if (handle < visible.size()) visible[handle] = true;
            }
            for (const SceneGrid& grid : scene.grids) {
                // The grid-level form of the same rule: a grid hidden behind a resolved result is
                // still fully readable geometry, and a ray must no more stop on it than on a hidden
                // loose chunk. See SceneGrid::rendered.
                if (!grid.rendered) continue;
                for (int32_t handle : grid.cellToChunk) {
                    if (handle >= 0 && size_t(handle) < visible.size()) visible[handle] = true;
                }
            }
            return visible;
        }
    }

    bool queryVoxelMaterial(const GeometryBlob& blob, uint32_t resolution,
                            core::ivec3 voxelCoord, uint8_t& materialSlotOut) {
        if (blob.geometry.empty() || resolution == 0) return false;
        if (voxelCoord.x < 0 || voxelCoord.y < 0 || voxelCoord.z < 0) return false;
        if (uint32_t(voxelCoord.x) >= resolution || uint32_t(voxelCoord.y) >= resolution ||
            uint32_t(voxelCoord.z) >= resolution) {
            return false;
        }

        uint64_t zOrder = createZOrderIndex(voxelCoord);

        // Depth is derived from the resolution the same way the shader derives it: six bits of
        // Z-order per level, so a level is 4x per axis.
        uint32_t treeLevels = uint32_t(std::log2(float(resolution)) / 2.0f);
        if (treeLevels == 0) return false;
        int level = int(treeLevels) - 1;
        uint64_t stepSize = 1ull << (6u * uint32_t(level));

        size_t nodeIndex = 0;
        for (; level >= 0; --level) {
            if ((nodeIndex * 3 + 2) >= blob.geometry.size()) return false;
            uint32_t mask1 = blob.geometry[nodeIndex * 3 + 0];
            uint32_t mask2 = blob.geometry[nodeIndex * 3 + 1];
            uint32_t data3 = blob.geometry[nodeIndex * 3 + 2];
            uint64_t occupancy = nodeOccupancy(mask1, mask2);
            uint32_t childPos = uint32_t((zOrder / stepSize) & 63ull);

            if (tree64IsLeaf(data3)) {
                // The leaf's mask says which of its 64 cells are solid. The shader skips this test
                // because it only asks about voxels a ray already hit; a pick has to ask.
                if (!childIsPresent(occupancy, childPos)) return false;

                uint32_t materialOffset = tree64LeafMatOffset(data3);
                // A uniform leaf stores one byte for all of its voxels rather than one each.
                uint32_t withinLeaf = tree64LeafIsUniform(data3) ? 0u : siblingsBefore(occupancy, childPos);
                size_t materialIndex = size_t(materialOffset) + withinLeaf;
                if (materialIndex >= blob.materialIDs.size()) return false;
                materialSlotOut = blob.materialIDs[materialIndex];
                return true;
            }

            if (!childIsPresent(occupancy, childPos)) return false;   // Empty space.

            uint32_t childPointer = data3 >> 1;
            nodeIndex = nodeIndex + childPointer + siblingsBefore(occupancy, childPos);
            stepSize >>= 6;
        }
        return false;
    }

    core::vec3 rayDirectionThroughImage(core::vec2 uv, core::vec2 resolution,
                                        core::vec3 cameraDirection, float verticalFovDegrees) {
        // Kept line-for-line equivalent to rayStartDirection in pjv_utils_DDA.sc. If that changes,
        // this has to change with it, or picks land next to what the user clicked.
        core::vec2 ndc = core::vec2(uv.x, 1.0f - uv.y) * 2.0f - 1.0f;
        float aspectRatio = resolution.y > 0.0f ? resolution.x / resolution.y : 1.0f;
        float scale = std::tan(glm::radians(verticalFovDegrees * 0.5f));

        core::vec3 forward = glm::normalize(cameraDirection);
        core::vec3 worldUp = std::abs(forward.y) > 0.999f ? core::vec3(0.0f, 0.0f, 1.0f)
                                                         : core::vec3(0.0f, 1.0f, 0.0f);
        core::vec3 right = glm::normalize(glm::cross(forward, worldUp));
        core::vec3 up = glm::normalize(glm::cross(right, forward));

        return glm::normalize(right * (ndc.x * scale * aspectRatio) + up * (ndc.y * scale) + forward);
    }

    VoxelPick pickVoxel(const Scene& scene, core::vec3 rayOrigin, core::vec3 rayDirection,
                        float maxDistance, const VoxelSolidityOverride& solidityOverride) {
        if (glm::length(rayDirection) <= 0.0f) return VoxelPick();
        core::vec3 direction = glm::normalize(rayDirection);

        // Chunks whose world box the ray crosses at all, nearest first. Everything else cannot
        // contain the first hit, and sorting is what lets the search stop early below.
        std::vector<bool> visible = visibleChunks(scene);
        std::vector<ChunkCandidate> candidates;
        for (ChunkHandle handle = 0; handle < scene.chunks.size(); handle++) {
            const Chunk& chunk = scene.chunks[handle];
            if (!visible[handle]) continue;
            if (!chunk.alive || chunk.header.scale <= 0.0f || chunk.geometryPoolIndex < 0) continue;

            // Into the chunk's frame: the header's rotation is about the chunk's own minimum
            // corner, which is the convention fetchVoxelColor uses.
            core::mat3 inverseRotation = glm::transpose(glm::mat3_cast(chunk.header.rotation));
            core::vec3 localOrigin = inverseRotation * (rayOrigin - chunk.header.position) + chunk.header.position;
            core::vec3 localDirection = inverseRotation * direction;

            core::vec3 inverseDirection = 1.0f / localDirection;   // Infinities here are intended.
            float entryDistance, exitDistance;
            intersectBox(localOrigin, inverseDirection, chunk.header.position,
                         chunk.header.position + core::vec3(chunk.header.scale),
                         entryDistance, exitDistance);

            if (exitDistance < entryDistance || exitDistance < 0.0f || entryDistance > maxDistance) continue;
            candidates.push_back({handle, std::max(entryDistance, 0.0f)});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const ChunkCandidate& a, const ChunkCandidate& b) { return a.entryDistance < b.entryDistance; });

        // The nearest voxel found so far, and how far along the ray it sits. `bestDistance` doubles
        // as the search cutoff: a candidate whose box the ray only enters *after* the best hit
        // cannot improve on it, and neither can anything sorted behind that candidate.
        //
        // Overlapping chunks are why this is a running minimum rather than a first-hit return. Box
        // entry order is not voxel hit order: a large, mostly-empty chunk whose box starts near the
        // camera loses to a small solid one sitting inside it, and returning on the first chunk that
        // yielded anything would report the far surface and let the ray pass straight through the
        // near one. The editor stacks exactly that shape -- a resolved result spanning a whole
        // assembly's bounding box, with its parts nested inside it.
        VoxelPick best;
        float bestDistance = maxDistance;

        for (const ChunkCandidate& candidate : candidates) {
            if (best.hit && candidate.entryDistance >= bestDistance) break;

            const Chunk& chunk = scene.chunks[candidate.handle];
            const GeometryBlob& blob = scene.geometryPool[chunk.geometryPoolIndex];
            uint32_t resolution = chunk.header.resolution;
            if (resolution == 0) continue;

            core::mat3 inverseRotation = glm::transpose(glm::mat3_cast(chunk.header.rotation));
            core::vec3 localOrigin = inverseRotation * (rayOrigin - chunk.header.position) + chunk.header.position;
            core::vec3 localDirection = inverseRotation * direction;

            float voxelSize = chunk.header.scale / float(resolution);
            // Enter the volume, nudged inside so the first cell is unambiguous on a face-on hit.
            core::vec3 entryPoint = localOrigin + localDirection * (candidate.entryDistance + voxelSize * 1e-3f);
            core::vec3 gridPosition = (entryPoint - chunk.header.position) / voxelSize;

            core::ivec3 voxel = core::ivec3(int(std::floor(gridPosition.x)),
                                            int(std::floor(gridPosition.y)),
                                            int(std::floor(gridPosition.z)));
            voxel = glm::clamp(voxel, core::ivec3(0), core::ivec3(int(resolution) - 1));

            // Amanatides & Woo: step one voxel at a time along whichever axis reaches its next
            // boundary first, in units of the ray parameter.
            core::ivec3 step;
            core::vec3 tMax, tDelta;
            for (int axis = 0; axis < 3; axis++) {
                float directionOnAxis = localDirection[axis];
                if (std::abs(directionOnAxis) < 1e-12f) {
                    step[axis] = 0;
                    tMax[axis] = std::numeric_limits<float>::infinity();
                    tDelta[axis] = std::numeric_limits<float>::infinity();
                    continue;
                }
                step[axis] = directionOnAxis > 0.0f ? 1 : -1;
                float nextBoundary = float(voxel[axis] + (step[axis] > 0 ? 1 : 0));
                tMax[axis] = (nextBoundary - gridPosition[axis]) / directionOnAxis * voxelSize;
                tDelta[axis] = voxelSize / std::abs(directionOnAxis);
            }

            // One traversal cannot visit more cells than the grid's diagonal reach.
            uint32_t stepBudget = resolution * 3u + 3u;
            float travelled = candidate.entryDistance;
            // Which axis the last DDA step crossed to arrive at the current cell, or -1 while still
            // on the cell the ray entered the chunk in. That axis *is* the face the ray came through,
            // known exactly and without any floating-point comparison -- see the hit branch below.
            int enteredAxis = -1;

            for (uint32_t stepIndex = 0; stepIndex < stepBudget; stepIndex++) {
                if (voxel.x < 0 || voxel.y < 0 || voxel.z < 0 ||
                    uint32_t(voxel.x) >= resolution || uint32_t(voxel.y) >= resolution ||
                    uint32_t(voxel.z) >= resolution) {
                    break;   // Left the chunk; the next candidate (if any) takes over.
                }
                // Past the cutoff: either the caller's reach, or a nearer hit already found in an
                // earlier candidate. Nothing further along this ray can win, so give up on this
                // chunk -- but keep the winner, rather than returning an empty pick over it.
                if (travelled > bestDistance) break;

                uint8_t materialSlot = 0;
                bool solid = queryVoxelMaterial(blob, resolution, voxel, materialSlot);
                // The override runs on every cell, not only on the solid ones: forcing a cell the
                // scene calls empty back to solid is half of what it is for (see the header).
                if (solidityOverride) {
                    solid = solidityOverride(candidate.handle, voxel, solid);
                }
                if (solid) {
                    VoxelPick pick;
                    pick.hit = true;
                    pick.chunk = candidate.handle;
                    pick.component = (chunk.gridIndex >= 0 && size_t(chunk.gridIndex) < scene.grids.size())
                                   ? scene.grids[chunk.gridIndex].componentHandle
                                   : chunk.componentHandle;
                    pick.voxelCoord = voxel;
                    pick.materialSlot = materialSlot;
                    pick.distance = travelled;

                    // The face the ray entered through, pointing back out at the ray. Stepping +1
                    // along an axis means entering through that axis's low face, whose outward
                    // normal is -1 -- hence the negation.
                    pick.faceNormal = core::ivec3(0);
                    if (enteredAxis >= 0) {
                        pick.faceNormal[enteredAxis] = -step[enteredAxis];
                    } else {
                        // Hit on the very first cell, which no DDA step led into: the ray came in
                        // through the chunk's own boundary. The entry point lies on that boundary, so
                        // the face is whichever axis it is flush against, and the normal opposes the
                        // ray on it. A ray that starts inside the volume has no entry face at all and
                        // correctly keeps the zero normal.
                        core::vec3 entryLocal = localOrigin + localDirection * candidate.entryDistance;
                        core::vec3 cellOffset = (entryLocal - chunk.header.position) / voxelSize -
                                                core::vec3(voxel);
                        int   flushestAxis = -1;
                        float smallestGap = 1e-3f;   // Must actually be on a boundary to count.
                        for (int axis = 0; axis < 3; axis++) {
                            if (step[axis] == 0) continue;
                            float gap = step[axis] > 0 ? std::abs(cellOffset[axis])
                                                       : std::abs(1.0f - cellOffset[axis]);
                            if (gap < smallestGap) {
                                smallestGap = gap;
                                flushestAxis = axis;
                            }
                        }
                        if (flushestAxis >= 0) {
                            pick.faceNormal[flushestAxis] = -step[flushestAxis];
                        }
                    }
                    // Centre of the hit voxel, rotated back out into world space.
                    core::vec3 localCentre = chunk.header.position +
                        (core::vec3(voxel) + core::vec3(0.5f)) * voxelSize;
                    core::mat3 rotation = glm::mat3_cast(chunk.header.rotation);
                    pick.worldPosition = rotation * (localCentre - chunk.header.position) + chunk.header.position;

                    // The nearest hit in this chunk, which is not necessarily the nearest in the
                    // scene -- so it is kept and compared, and the cutoff tightens onto it.
                    if (!best.hit || travelled < bestDistance) {
                        best = pick;
                        bestDistance = travelled;
                    }
                    break;   // This chunk is done; a nearer voxel can only be in another one.
                }

                // Advance across the nearest voxel boundary.
                if (tMax.x < tMax.y && tMax.x < tMax.z) {
                    voxel.x += step.x;
                    travelled = candidate.entryDistance + tMax.x;
                    tMax.x += tDelta.x;
                    enteredAxis = 0;
                } else if (tMax.y < tMax.z) {
                    voxel.y += step.y;
                    travelled = candidate.entryDistance + tMax.y;
                    tMax.y += tDelta.y;
                    enteredAxis = 1;
                } else {
                    voxel.z += step.z;
                    travelled = candidate.entryDistance + tMax.z;
                    tMax.z += tDelta.z;
                    enteredAxis = 2;
                }
            }
        }

        return best;
    }
}

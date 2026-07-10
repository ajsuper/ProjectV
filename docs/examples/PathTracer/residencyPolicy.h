#ifndef PATHTRACER_RESIDENCY_POLICY_H
#define PATHTRACER_RESIDENCY_POLICY_H

// EXAMPLE streaming residency policy — application code, NOT engine behavior.
//
// ProjectV's engine ships streaming *mechanisms* (per-block IO, materialize/release a grid cell, the
// mutation-apply seam) but deliberately no residency *policy*: deciding which cells should be resident is
// the user's call, exactly like choosing a renderer module. This file is one such policy — a basic
// distance rule — that the PathTracer example opts into. A real game would replace it with its own rule
// (portals, prediction, server-authoritative interest) on the same mechanisms in utils/streaming.h.
//
// It is driven by a LIST of interest sources, not a single camera, so split-screen / multiple cameras /
// networked players just push more sources. See docs/data_structures/compose_data_structure.md.
//
// Distance is measured PER CELL and compared to a multiple of THAT cell's size, not a fixed world radius.
// This matters because a Compose scene can mix grids whose cellSize spans orders of magnitude (e.g. the
// fractal SponzaScene). A cell is loaded when a source is within `loadCells * cellSize` of it — so coarse
// cells stream in from far away while fine detail only appears up close (a natural screen-size LOD rule).
// A fixed world radius would either miss the big coarse grids or drown in invisible sub-pixel ones.

#include <vector>
#include <algorithm>

#include <glm/gtc/quaternion.hpp>

#include "core/math.h"
#include "data_structures/scene.h"
#include "data_structures/compose.h"
#include "data_structures/gpuData.h"
#include "graphics/scene_dynamics.h"
#include "utils/streaming.h"

// A point that pulls nearby grid cells into residency (one per camera / player / point of interest).
struct InterestSource {
    projv::core::vec3 position;
};

// Residency reach is the MAX of an absolute world radius and a per-cell-size multiple:
//   loadDist = max(loadRadius, loadCells * cellSize)
// The absolute term covers the whole model near the camera (so nothing is missing at the visible scale,
// matching an eager load); the cell-size term additionally keeps coarse, far-away structure resident
// without dragging in invisible sub-pixel detail. A fractal/self-similar scene needs both: a pure
// absolute radius misses coarse grids beyond it, and a pure cell-size radius never reaches fine detail
// that is far away but still drawn. Tune loadRadius up to see more of the model at once.
struct ResidencyPolicy {
    // PURE world-space distance: a cell is loaded when a source is within loadRadius of its centre, and
    // evicted once beyond evictRadius. Same metric for load and evict (evictRadius > loadRadius gives
    // hysteresis), so unloading is even and symmetric with loading — no per-cell-size weighting. Raise
    // loadRadius to see more at once; a tight radius makes a streaming bubble that fills in as you move.
    float loadRadius  = 3000.0f;
    float evictRadius = 4000.0f;
    int   maxLoadsPerFrame  = 4096;
    int   maxEvictsPerFrame = 8192;
};

// Reflect the interest sources onto the scene: materialize occupied grid cells within loadCells*cellSize
// of any source, evict resident cells beyond evictCells*cellSize of every source, then apply the batch
// (one GPU-table sync for the whole frame). Far grids are culled by their bounding sphere so a frame does
// not scan every grid's block table. Nothing here loads geometry itself — that is the mechanisms' job.
inline void updateSceneResidency(projv::Scene& scene, projv::GPUData& gpuData,
                                 projv::utils::StreamingContext& streaming,
                                 const std::vector<InterestSource>& sources,
                                 const ResidencyPolicy& policy = {}) {
    using namespace projv;
    if (sources.empty()) return;
    int loads = 0, evicts = 0;

    for (int gi = 0; gi < static_cast<int>(scene.grids.size()); gi++) {
        SceneGrid& g = scene.grids[gi];
        if (g.cellSize <= 0.0f) continue;
        core::mat3 rot = glm::mat3_cast(g.rotation);

        // Pure world-space distances (see ResidencyPolicy).
        float loadDist  = policy.loadRadius;
        float evictDist = policy.evictRadius;

        // Cull grids that are entirely out of evict range of every source (cheap bounding sphere test),
        // so we only read block tables for grids that can actually have loads/evicts this frame.
        core::vec3 gridCenter = g.origin + rot * (core::vec3(g.dims.x, g.dims.y, g.dims.z) * (0.5f * g.cellSize));
        float gridRadius = 0.5f * glm::length(core::vec3(g.dims.x, g.dims.y, g.dims.z)) * g.cellSize;
        bool relevant = false;
        for (const InterestSource& s : sources)
            if (glm::length(gridCenter - s.position) - gridRadius <= evictDist) { relevant = true; break; }
        if (!relevant) continue;
        const DataFileHeader& table = utils::streamGridBlockTable(streaming, scene, gi);
        for (const BlockEntry& e : table.blocks) {
            if (e.gridX < 0 || e.gridY < 0 || e.gridZ < 0) continue;
            int cell = e.gridX + g.dims.x * (e.gridY + g.dims.y * e.gridZ);
            if (cell < 0 || cell >= static_cast<int>(g.cellToChunk.size())) continue;

            // World-space centre of this cell (corner + half a cell along the grid's rotated axes).
            core::vec3 local = (core::vec3(static_cast<float>(e.gridX), static_cast<float>(e.gridY),
                                           static_cast<float>(e.gridZ)) + 0.5f) * g.cellSize;
            core::vec3 center = g.origin + rot * local;

            float best = 1e30f;
            for (const InterestSource& s : sources) best = std::min(best, glm::length(center - s.position));

            bool resident = g.cellToChunk[cell] >= 0;
            if (!resident && best <= loadDist && loads < policy.maxLoadsPerFrame) {
                utils::materializeGridCell(scene, streaming, gi, cell);
                loads++;
            } else if (resident && best > evictDist && evicts < policy.maxEvictsPerFrame) {
                utils::releaseGridCell(scene, streaming, gi, cell);
                evicts++;
            }
        }
    }

    projv::graphics::applySceneMutations(scene, gpuData, streaming.mutations);
}

#endif

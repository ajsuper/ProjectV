#ifndef PROJECTV_TEARDOWN_WRITER_H
#define PROJECTV_TEARDOWN_WRITER_H
// A converted level, written as a ProjectV compose folder:
//
//   <out>/<level>/compose.json           the scene: the world grid and one entry per dynamic body
//   <out>/<level>/entities.json          physics.static on the world, physics.rigidbody on bodies
//   <out>/<level>/physics_materials.json Teardown's substances as a physics material library
//   <out>/<level>/world.data             static geometry, a grid of 64^3 tree64 blocks
//   <out>/<level>/water.data             water surfaces, translucent, 0.4 m voxels
//   <out>/<level>/bodies/*.data          one per distinct dynamic body (identical props share one)
//
// The geometry goes through the engine's own builders (brick map -> tree64 -> baked material
// bytes -> PVDT writer), so the files are exactly what ProjectV writes itself.

#include <string>

#include "level.h"

namespace teardown {

struct WriteOptions {
    int blockResolution = 64;     // world grid cell edge, in voxels (a power of 4)
    bool physicsNames = true;     // name each palette entry's substance as a physics material
    bool entities = true;         // write entities.json
    int threads = 0;              // 0 = hardware concurrency
    // World units per metre. ProjectV's own scenes are one unit per voxel, so the default makes a
    // Teardown voxel (0.1 m) one unit. Applied in compose.json as each component's uniform scale,
    // with positions multiplied to match; the .data files stay in metres.
    float scale = 10.0f;
};

struct WriteReport {
    size_t worldVoxels = 0, worldBlocks = 0;
    size_t bodies = 0, bodyFiles = 0, bodyVoxels = 0;
    size_t waterVoxels = 0;
    size_t worldMaterials = 0;    // distinct materials before quantisation
    double worldPaletteError = 0; // largest colour shift quantisation made (Oklab distance)
    size_t bytes = 0;
};

// Multiplies every top-level component of a compose.json by `factor` about the origin: positions
// and scales alike, so the scene keeps its layout. For folders already converted (--td-rescale).
bool rescaleCompose(const std::string& folder, float factor);

bool writeLevel(const Level& level, const LevelSource& src, const std::string& folder, const WriteOptions& options,
                WriteReport& report);

} // namespace teardown

#endif // PROJECTV_TEARDOWN_WRITER_H

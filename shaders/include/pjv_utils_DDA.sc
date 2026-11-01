#ifndef PJV_UTILS_DDA_SC
#define PJV_UTILS_DDA_SC

// Temporary, while shader-library S5 moves every shader onto the files it uses: the traversal is
// pjv_scene.sc, the materials pjv_material.sc, the motion table pjv_motion.sc. This file goes once
// nothing includes it (docs/plans/shader-library.md).

// World-unit scale. Voxels are now 1 world unit (voxelScale = 1); the shaders were tuned for
// ~0.0727-unit voxels, so the scene is ~13.76x larger. Multiply world-space DISTANCE constants
// (self-hit ray-origin offsets, ray lengths) by this; leave voxel-relative / step-count values.
#ifndef WORLD_SCALE
#define WORLD_SCALE 13.76
#endif

#include <pjv_camera.sc>
#include <pjv_scene.sc>

#endif

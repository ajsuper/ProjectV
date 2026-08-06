#ifndef PROJECTV_PICKING_H
#define PROJECTV_PICKING_H

#include <cstdint>
#include <functional>

#include "core/math.h"
#include "data_structures/scene.h"

namespace projv::utils {

    // What a ray hit. `hit` false means the ray left the scene without touching a voxel; every other
    // field is meaningless in that case.
    struct VoxelPick {
        bool            hit = false;
        ChunkHandle     chunk = 0;                              // Which chunk the voxel lives in.
        ComponentHandle component = INVALID_COMPONENT_HANDLE;   // Which component owns that chunk.
        core::ivec3     voxelCoord = core::ivec3(0);            // Voxel coordinate inside the chunk.
        // Outward normal of the face the ray entered through, in the chunk's own voxel grid: exactly
        // one component is +/-1, the other two are 0. This is what an additive brush needs -- the
        // empty cell to fill is `voxelCoord + faceNormal`, with no guessing about which side of the
        // voxel was struck. For a world-space normal, rotate it:
        //     mat3_cast(chunk.header.rotation) * vec3(faceNormal)
        core::ivec3     faceNormal = core::ivec3(0);
        core::vec3      worldPosition = core::vec3(0.0f);       // Centre of the hit voxel, in world space.
        float           distance = 0.0f;                        // Along the ray, from its origin.
        uint8_t         materialSlot = 0;                       // Slot in the component's palette.
    };

    /**
     * Reads the material of a single voxel straight out of a blob's tree64, without touching the GPU.
     * Mirrors the descent the shader does in fetchVoxelColor, plus the emptiness test the shader can
     * skip (it only ever asks about voxels a ray already hit).
     *
     * @param blob The geometry to query.
     * @param resolution The chunk's native resolution (must be a power of four, as the tree depth is
     *                   derived from it exactly as the shader derives it).
     * @param voxelCoord Coordinate within the chunk, in [0, resolution).
     * @param materialSlotOut Receives the palette slot when the voxel exists.
     * @return True when a voxel exists at that coordinate.
     */
    bool queryVoxelMaterial(const GeometryBlob& blob, uint32_t resolution,
                            core::ivec3 voxelCoord, uint8_t& materialSlotOut);

    /**
     * Lets a caller decide, voxel by voxel, what the ray treats as solid — overriding what the scene
     * actually holds. Called for every cell the traversal steps into, with what the geometry says, and
     * returning what the ray should believe.
     *
     * This exists for edits made *during* a drag. A sculpt stroke rebuilds the scene between frames,
     * so by the next frame the ray would hit the geometry the stroke just laid down: an additive drag
     * would climb its own deposits back toward the camera, and a subtractive one would tunnel through
     * the hole it just opened. Neither is a bug in the ray — the scene really does hold that. The fix
     * is to make the ray see the surface the stroke *started* against, for as long as the button is
     * held, which is one predicate over the stroke's own set of touched voxels:
     *
     *     added this stroke   -> report empty  (the ray passes through, and keeps hitting the original
     *                                           surface, so the stroke stays where the user aimed it)
     *     removed this stroke -> report solid  (the ray still stops there, so the brush does not sink)
     *
     * A voxel forced solid has no material to report: it is not in the tree any more. Such a pick
     * carries materialSlot 0, so anything that reads the slot (a sampler, an eyedropper) must not be
     * given an override that forces solidity.
     *
     * @param chunk The chunk the voxel belongs to.
     * @param voxelCoord Its coordinate within that chunk.
     * @param solidInScene What the geometry says: true when a voxel is really there.
     * @return What the ray should treat the cell as.
     */
    using VoxelSolidityOverride =
        std::function<bool(ChunkHandle chunk, core::ivec3 voxelCoord, bool solidInScene)>;

    /**
     * Casts a ray through the scene on the CPU and returns the **nearest** voxel it hits.
     *
     * This is deliberately not the GPU's job: a pick is one ray on a user action, and reading a
     * G-buffer back stalls the pipeline (and needs the renderer to have a G-buffer at all, which the
     * editor's viewport renderer does not). Chunks are tested against their world bounding boxes,
     * nearest first, and each survivor is walked voxel by voxel — so cost scales with the chunk's
     * resolution along the ray, not with the size of the scene.
     *
     * Two properties callers depend on, both of which cost nothing when chunks do not overlap:
     *
     * - **Only what is drawn is hit.** The set tested is the set the renderer reaches — the scene's
     *   loose list plus every grid cell — not every chunk that happens to be alive. The two differ
     *   whenever something is hidden by un-listing it (which is how a caller hides geometry that must
     *   stay readable: see SceneEditor's setComponentRendered). Without this a ray stops on surfaces
     *   that are not on screen, and a click lands on something the user cannot see.
     * - **Nearest wins, not first.** Bounding-box entry order is not voxel hit order, so a chunk
     *   whose box is entered early but whose content is far cannot claim a hit over a nearer voxel in
     *   a chunk tested later. This is what makes a pick well defined when chunks overlap or nest.
     *
     * @param scene The scene to cast into.
     * @param rayOrigin World-space start of the ray (typically the camera).
     * @param rayDirection World-space direction; need not be normalized.
     * @param maxDistance How far to look before giving up.
     * @param solidityOverride Optional; see VoxelSolidityOverride. Empty means "believe the scene".
     * @return The hit, or a VoxelPick with hit == false.
     */
    VoxelPick pickVoxel(const Scene& scene, core::vec3 rayOrigin, core::vec3 rayDirection,
                        float maxDistance = 1.0e6f,
                        const VoxelSolidityOverride& solidityOverride = VoxelSolidityOverride());

    /**
     * Builds the world-space ray through a point on a rendered image, matching the ray generation in
     * pjv_utils_DDA.sc (rayStartDirection) exactly — so what the editor picks is what the viewport
     * drew under the cursor.
     *
     * @param uv Position within the image, [0,1] from its top-left corner.
     * @param resolution The image's pixel resolution (its aspect ratio is what matters).
     * @param cameraDirection Camera forward; need not be normalized.
     * @param verticalFovDegrees Vertical field of view — 60 for every renderer shipped so far.
     * @return A normalized world-space direction.
     */
    core::vec3 rayDirectionThroughImage(core::vec2 uv, core::vec2 resolution,
                                        core::vec3 cameraDirection, float verticalFovDegrees = 60.0f);
}

#endif

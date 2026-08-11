#ifndef PROJECTV_MATERIAL_H
#define PROJECTV_MATERIAL_H

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

#include "data_structures/color.h"
#include "data_structures/scene.h"
#include "data_structures/voxel.h"

namespace projv::utils {

// Thread-safe: locks scene.materialPaletteMutex around the whole read-modify-write (the linear
// scans, the push_back, and the paletteVersion bump), so worker threads can intern materials
// directly instead of needing a thread-local-dedup-then-merge workaround. Safe to call
// concurrently, but not free -- callers doing high-frequency interning (e.g. per-voxel) should
// still dedupe locally first for performance; the lock buys correctness, not throughput.
uint8_t internMaterial(Scene& scene, ComponentRecord& comp, const std::string& name, uint32_t packedColor);
uint8_t findMaterialByName(const ComponentRecord& comp, const std::string& name);
uint8_t findMaterialByColor(const ComponentRecord& comp, uint32_t packedColor);

// --- Palette editing -------------------------------------------------------------------------
//
// The interning path above exists to *build* a palette while voxelizing: it dedupes, so asking for
// a colour that is already there hands back the slot that has it. Editing a loaded scene is the
// opposite problem -- the palette is the thing being changed, and a change has to be told apart
// from a lookup. Hence this set. All of them bump ComponentRecord::paletteVersion, which is what
// makes the GPU notice (see rebuildGlobalPaletteTexture / updatePaletteEntry in gpu_interface).
//
// Slots are per-component and are what the voxel data stores, so their *numbering* is data:
// recolouring a slot is free, but removing one renumbers every slot above it and every voxel that
// referenced them. removeMaterial is the only one of these that touches geometry.

/**
 * Appends a palette entry without deduping — the "add a new material" action, as distinct from
 * internMaterial's "give me the slot for this colour".
 * @param scene The scene owning the component (locked for the palette mutation).
 * @param componentHandle The component whose palette gains the entry.
 * @param name Name for the new entry; may be empty.
 * @param packedColor R10G10B10 colour (see packColor in voxel.h).
 * @return The new slot, or INVALID_MATERIAL if the handle is bad or the palette is full.
 */
uint8_t addMaterial(Scene& scene, ComponentHandle componentHandle, const std::string& name, uint32_t packedColor);

/**
 * Recolours an existing slot. Every voxel referencing it changes colour; nothing else moves, which
 * is why this is the cheap edit — no geometry is touched and slot numbering is untouched.
 * @return False if the handle or slot is out of range.
 */
bool setMaterialColor(Scene& scene, ComponentHandle componentHandle, uint8_t slot, uint32_t packedColor);

/**
 * Rewrites a slot's non-colour property words — emission, glossiness, metallic, transparency, IOR
 * and the rest of Material's four-word payload (see the Material comment in scene.h).
 *
 * Size-preserving in exactly the sense setMaterialColor is: it changes what a slot means, never how
 * many slots exist, so it takes the same cheap single-texel GPU route via updatePaletteEntry rather
 * than a full flush. Split from setMaterialColor rather than folded into it because an editor drags
 * one or the other, and a caller that only moved a slider should not have to restate the colour.
 *
 * @param scene The scene owning the component.
 * @param componentHandle The component whose palette is edited.
 * @param slot The slot to rewrite.
 * @param packedEmission Emission colour word (word1).
 * @param packedSurface Glossiness/metallic/transparency/IOR word (word2).
 * @param packedExtra Emissive-strength/transmission/flags word (word3).
 * @return False if the handle or slot is out of range.
 */
bool setMaterialProperties(Scene& scene, ComponentHandle componentHandle, uint8_t slot,
                           uint32_t packedEmission, uint32_t packedSurface, uint32_t packedExtra);

/**
 * Renames an existing slot. Names are how compose data and internMaterial identify materials, so
 * this is not purely cosmetic — two entries sharing a name will collide in internMaterial.
 * @return False if the handle or slot is out of range.
 */
bool setMaterialName(Scene& scene, ComponentHandle componentHandle, uint8_t slot, const std::string& name);

/**
 * Counts how many voxels in the scene reference each of a component's palette slots.
 * Shared geometry is counted once per referencing chunk, so the result is voxels *in the scene*,
 * not voxels in the data — an instanced blob used by 100 cells counts 100 times.
 * @return One count per palette slot (empty if the handle is bad).
 */
std::vector<uint32_t> countMaterialUsage(const Scene& scene, ComponentHandle componentHandle);

// Where one slot's voxels are: a chunk, and how many of its voxels use the slot.
struct MaterialChunkUsage {
    ChunkHandle chunk;
    uint32_t    voxelCount;
    core::vec3  chunkPosition;   // World position of the chunk's minimum corner.
};

/**
 * Breaks one slot's usage down by chunk, so "where is this material actually used" has an answer
 * beyond a total. Chunks that do not use the slot are omitted; the result is sorted by count,
 * heaviest first.
 * @param scene The scene to inspect.
 * @param componentHandle The component owning the palette.
 * @param slot The palette slot to locate.
 */
std::vector<MaterialChunkUsage> findMaterialChunks(const Scene& scene, ComponentHandle componentHandle,
                                                   uint8_t slot);

/**
 * Removes a palette slot, reassigning any voxels that used it and renumbering the slots above it
 * across all of the component's geometry. This rewrites materialIDs in every blob the component
 * owns and marks them dirty, so the caller must follow with graphics::flushSceneUpdates.
 *
 * A blob shared with chunks outside this component is copied first (the other component's voxels
 * keep the old numbering), mirroring forkBlob's copy-on-write.
 *
 * @param scene The scene to edit.
 * @param componentHandle The component whose palette shrinks.
 * @param slot The slot to remove.
 * @param replacementSlot Slot the removed slot's voxels become. Ignored when nothing uses `slot`;
 *                        required (and must differ from `slot`) when something does.
 * @return False if the handle/slot is out of range, if `slot` is the palette's only entry, or if
 *         the slot is in use and `replacementSlot` is not a valid different slot.
 */
bool removeMaterial(Scene& scene, ComponentHandle componentHandle, uint8_t slot, uint8_t replacementSlot);

// -----------------------------------------------------------------------------------------------

void brickMapSetMaterial(BrickData& brick, uint32_t localZOrder, uint8_t materialID);
uint8_t brickMapGetMaterial(const BrickData& brick, uint32_t localZOrder);

void bakeMaterialsFromBrickMap(std::vector<uint32_t>& geometry,
                                std::vector<uint8_t>& materialIDs,
                                const VoxelBrickMap& map);

} // namespace projv::utils

#endif
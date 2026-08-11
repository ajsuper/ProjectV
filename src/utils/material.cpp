#include <algorithm>
#include "utils/material.h"
#include "core/log.h"

namespace projv::utils {

uint8_t internMaterial(Scene& scene, ComponentRecord& comp, const std::string& name, uint32_t packedColor) {
    std::lock_guard<std::mutex> lock(scene.materialPaletteMutex);
    if (!name.empty()) {
        for (size_t i = 0; i < comp.materialPalette.size(); ++i) {
            if (comp.materialPalette[i].name == name) {
                return static_cast<uint8_t>(i);
            }
        }
    }
    for (size_t i = 0; i < comp.materialPalette.size(); ++i) {
        if (comp.materialPalette[i].packedColor == packedColor) {
            return static_cast<uint8_t>(i);
        }
    }
    // Hard cap: material IDs are uint8_t, and 255 is INVALID_MATERIAL. Past this point the
    // static_cast below would wrap and the new material would silently alias palette slot
    // (index & 255), corrupting the colors of whatever already owns that slot. Fail loudly
    // instead -- this used to be an assert, which is compiled out in release builds.
    if (comp.materialPalette.size() >= MAX_MATERIALS_PER_COMPONENT - 1) {
        core::error("internMaterial: palette full ({} entries) -- cannot add '{}' (0x{:06X}). "
                    "Material IDs are uint8_t; quantize or reduce the color set.",
                    comp.materialPalette.size(), name, packedColor);
        return INVALID_MATERIAL;
    }
    Material mat;
    mat.name = name;
    mat.packedColor = packedColor;
    comp.materialPalette.push_back(mat);
    comp.paletteVersion++;
    return static_cast<uint8_t>(comp.materialPalette.size() - 1);
}

uint8_t findMaterialByName(const ComponentRecord& comp, const std::string& name) {
    for (size_t i = 0; i < comp.materialPalette.size(); ++i) {
        if (comp.materialPalette[i].name == name) {
            return static_cast<uint8_t>(i);
        }
    }
    return INVALID_MATERIAL;
}

uint8_t findMaterialByColor(const ComponentRecord& comp, uint32_t packedColor) {
    for (size_t i = 0; i < comp.materialPalette.size(); ++i) {
        if (comp.materialPalette[i].packedColor == packedColor) {
            return static_cast<uint8_t>(i);
        }
    }
    return INVALID_MATERIAL;
}

// --- Palette editing -------------------------------------------------------------------------

namespace {
    // Which component a chunk's voxels belong to. A grid-resident chunk takes its identity from the
    // grid (one component owns every cell); a loose chunk carries its own.
    ComponentHandle componentOfChunk(const Scene& scene, const Chunk& chunk) {
        if (chunk.gridIndex >= 0 && static_cast<size_t>(chunk.gridIndex) < scene.grids.size()) {
            return scene.grids[chunk.gridIndex].componentHandle;
        }
        return chunk.componentHandle;
    }

    // Every pool blob the component's live chunks reference, with how many of those references come
    // from this component. A blob whose total refCount exceeds that is shared with someone else and
    // cannot be rewritten in place.
    std::unordered_map<int32_t, uint32_t> blobsOfComponent(const Scene& scene, ComponentHandle componentHandle) {
        std::unordered_map<int32_t, uint32_t> references;
        for (const Chunk& chunk : scene.chunks) {
            if (!chunk.alive || chunk.geometryPoolIndex < 0) continue;
            if (componentOfChunk(scene, chunk) != componentHandle) continue;
            references[chunk.geometryPoolIndex]++;
        }
        return references;
    }

    // Voxels per palette slot in one blob.
    //
    // Counting the materialIDs array directly would count *bytes*, not voxels: a uniform leaf (every
    // set voxel in it sharing a material) stores exactly one byte for up to 64 voxels, and on
    // heightfield terrain ~96% of leaves are uniform. So the tree is walked instead, and each leaf
    // contributes the number of voxels its occupancy mask actually has.
    void accumulateBlobUsage(const GeometryBlob& blob, std::vector<uint32_t>& usage) {
        size_t nodeCount = blob.geometry.size() / 3;
        for (size_t node = 0; node < nodeCount; node++) {
            uint32_t data3 = blob.geometry[node * 3 + 2];
            if (!tree64IsLeaf(data3)) continue;

            uint64_t occupancy = (uint64_t(blob.geometry[node * 3 + 0]) << 32) |
                                  uint64_t(blob.geometry[node * 3 + 1]);
            uint32_t voxelsInLeaf = uint32_t(__builtin_popcountll(occupancy));
            uint32_t materialOffset = tree64LeafMatOffset(data3);

            if (tree64LeafIsUniform(data3)) {
                if (materialOffset >= blob.materialIDs.size()) continue;
                uint8_t slot = blob.materialIDs[materialOffset];
                if (slot < usage.size()) usage[slot] += voxelsInLeaf;
                continue;
            }
            // A non-uniform leaf stores one byte per set voxel, in mask order.
            for (uint32_t i = 0; i < voxelsInLeaf; i++) {
                size_t materialIndex = size_t(materialOffset) + i;
                if (materialIndex >= blob.materialIDs.size()) break;
                uint8_t slot = blob.materialIDs[materialIndex];
                if (slot < usage.size()) usage[slot]++;
            }
        }
    }

    // Applies a 256-entry slot remap to a blob's baked material bytes and, when one exists, to the
    // brick map behind them. The brick map is the source of truth for later voxel edits, so leaving
    // it on the old numbering would resurrect the removed slot the next time the blob is rebaked.
    void remapBlobMaterials(GeometryBlob& blob, const uint8_t (&slotRemap)[256]) {
        for (uint8_t& materialID : blob.materialIDs) {
            materialID = slotRemap[materialID];
        }
        if (!blob.brickMap) return;
        for (std::unique_ptr<BrickData>& brick : blob.brickMap->bricks) {
            if (!brick) continue;
            for (std::pair<const uint32_t, uint8_t>& voxel : brick->materials) {
                voxel.second = slotRemap[voxel.second];
            }
        }
    }
}

uint8_t addMaterial(Scene& scene, ComponentHandle componentHandle, const std::string& name, uint32_t packedColor) {
    if (componentHandle >= scene.components.size()) {
        core::error("addMaterial: component handle {} out of range ({} components).",
                    componentHandle, scene.components.size());
        return INVALID_MATERIAL;
    }
    std::lock_guard<std::mutex> lock(scene.materialPaletteMutex);
    ComponentRecord& comp = scene.components[componentHandle];

    // Same hard cap as internMaterial: IDs are uint8_t and 255 is INVALID_MATERIAL, so slot 254 is
    // the last usable one.
    if (comp.materialPalette.size() >= MAX_MATERIALS_PER_COMPONENT - 1) {
        core::error("addMaterial: palette full ({} entries) -- cannot add '{}' (0x{:06X}).",
                    comp.materialPalette.size(), name, packedColor);
        return INVALID_MATERIAL;
    }

    Material material;
    material.name = name;
    material.packedColor = packedColor;
    comp.materialPalette.push_back(material);
    comp.paletteVersion++;
    return static_cast<uint8_t>(comp.materialPalette.size() - 1);
}

bool setMaterialColor(Scene& scene, ComponentHandle componentHandle, uint8_t slot, uint32_t packedColor) {
    if (componentHandle >= scene.components.size()) return false;
    std::lock_guard<std::mutex> lock(scene.materialPaletteMutex);
    ComponentRecord& comp = scene.components[componentHandle];
    if (slot >= comp.materialPalette.size()) return false;

    if (comp.materialPalette[slot].packedColor == packedColor) return true;
    comp.materialPalette[slot].packedColor = packedColor;
    comp.paletteVersion++;
    return true;
}

bool setMaterialProperties(Scene& scene, ComponentHandle componentHandle, uint8_t slot,
                           uint32_t packedEmission, uint32_t packedSurface, uint32_t packedExtra) {
    if (componentHandle >= scene.components.size()) return false;
    std::lock_guard<std::mutex> lock(scene.materialPaletteMutex);
    ComponentRecord& comp = scene.components[componentHandle];
    if (slot >= comp.materialPalette.size()) return false;

    Material& material = comp.materialPalette[slot];
    if (material.packedEmission == packedEmission && material.packedSurface == packedSurface &&
        material.packedExtra == packedExtra) {
        return true;
    }
    material.packedEmission = packedEmission;
    material.packedSurface = packedSurface;
    material.packedExtra = packedExtra;
    comp.paletteVersion++;
    return true;
}

bool setMaterialName(Scene& scene, ComponentHandle componentHandle, uint8_t slot, const std::string& name) {
    if (componentHandle >= scene.components.size()) return false;
    std::lock_guard<std::mutex> lock(scene.materialPaletteMutex);
    ComponentRecord& comp = scene.components[componentHandle];
    if (slot >= comp.materialPalette.size()) return false;

    if (comp.materialPalette[slot].name == name) return true;
    comp.materialPalette[slot].name = name;
    // A name change moves no colour, but the version is what every consumer watches, and a stale
    // name is still a stale palette.
    comp.paletteVersion++;
    return true;
}

std::vector<uint32_t> countMaterialUsage(const Scene& scene, ComponentHandle componentHandle) {
    if (componentHandle >= scene.components.size()) return {};
    const ComponentRecord& comp = scene.components[componentHandle];
    std::vector<uint32_t> usage(comp.materialPalette.size(), 0);

    // Counted per unique blob and multiplied by how many of the component's chunks reference it,
    // rather than walked once per chunk: an instanced grid can share three blobs across a thousand
    // cells, and the arithmetic gives the same answer for a thousandth of the work.
    for (const std::pair<const int32_t, uint32_t>& reference : blobsOfComponent(scene, componentHandle)) {
        std::vector<uint32_t> blobUsage(usage.size(), 0);
        accumulateBlobUsage(scene.geometryPool[reference.first], blobUsage);
        for (size_t slot = 0; slot < usage.size(); slot++) {
            usage[slot] += blobUsage[slot] * reference.second;
        }
    }
    return usage;
}

std::vector<MaterialChunkUsage> findMaterialChunks(const Scene& scene, ComponentHandle componentHandle,
                                                   uint8_t slot) {
    std::vector<MaterialChunkUsage> usage;
    if (componentHandle >= scene.components.size()) return usage;

    // Counted once per blob, then attributed to each chunk that shares it — instances of the same
    // geometry have identical material bytes, so counting per chunk would be the same work repeated.
    std::unordered_map<int32_t, uint32_t> countPerBlob;
    size_t paletteSize = scene.components[componentHandle].materialPalette.size();
    for (const std::pair<const int32_t, uint32_t>& reference : blobsOfComponent(scene, componentHandle)) {
        std::vector<uint32_t> blobUsage(paletteSize, 0);
        accumulateBlobUsage(scene.geometryPool[reference.first], blobUsage);
        countPerBlob[reference.first] = slot < blobUsage.size() ? blobUsage[slot] : 0;
    }

    for (ChunkHandle handle = 0; handle < scene.chunks.size(); handle++) {
        const Chunk& chunk = scene.chunks[handle];
        if (!chunk.alive || chunk.geometryPoolIndex < 0) continue;
        if (componentOfChunk(scene, chunk) != componentHandle) continue;

        auto it = countPerBlob.find(chunk.geometryPoolIndex);
        if (it == countPerBlob.end() || it->second == 0) continue;
        usage.push_back({handle, it->second, chunk.header.position});
    }

    std::sort(usage.begin(), usage.end(),
              [](const MaterialChunkUsage& a, const MaterialChunkUsage& b) { return a.voxelCount > b.voxelCount; });
    return usage;
}

bool removeMaterial(Scene& scene, ComponentHandle componentHandle, uint8_t slot, uint8_t replacementSlot) {
    if (componentHandle >= scene.components.size()) {
        core::error("removeMaterial: component handle {} out of range.", componentHandle);
        return false;
    }
    size_t paletteSize = scene.components[componentHandle].materialPalette.size();
    if (slot >= paletteSize) {
        core::error("removeMaterial: slot {} out of range ({} entries).", slot, paletteSize);
        return false;
    }
    if (paletteSize == 1) {
        // An empty palette would leave every voxel of the component pointing at nothing, and there
        // is no slot left to reassign them to.
        core::error("removeMaterial: refusing to remove the palette's only entry.");
        return false;
    }

    std::vector<uint32_t> usage = countMaterialUsage(scene, componentHandle);
    bool slotIsUsed = usage[slot] > 0;
    if (slotIsUsed && (replacementSlot == slot || replacementSlot >= paletteSize)) {
        core::error("removeMaterial: slot {} is used by {} voxel(s) and needs a valid replacement "
                    "slot (got {}).", slot, usage[slot], replacementSlot);
        return false;
    }

    // Slot numbering after the erase: everything above the removed slot shifts down one, and the
    // removed slot's voxels become the replacement (itself shifted, if it was above).
    uint8_t remappedReplacement = replacementSlot > slot ? uint8_t(replacementSlot - 1) : replacementSlot;
    uint8_t slotRemap[256];
    for (int id = 0; id < 256; id++) {
        if (id == INVALID_MATERIAL) {
            slotRemap[id] = INVALID_MATERIAL;   // The "no material" sentinel is not a slot.
        } else if (id == slot) {
            slotRemap[id] = remappedReplacement;
        } else if (id > slot) {
            slotRemap[id] = uint8_t(id - 1);
        } else {
            slotRemap[id] = uint8_t(id);
        }
    }

    for (const std::pair<const int32_t, uint32_t>& reference : blobsOfComponent(scene, componentHandle)) {
        int32_t poolIndex = reference.first;
        uint32_t referencesFromThisComponent = reference.second;
        GeometryBlob& blob = scene.geometryPool[poolIndex];

        if (blob.refCount == referencesFromThisComponent) {
            remapBlobMaterials(blob, slotRemap);
            blob.dirty = true;
            continue;
        }

        // Shared with chunks outside this component, whose voxels still mean the old numbering.
        // Copy-on-write, exactly as an edit to shared geometry would (see forkBlob).
        GeometryBlob forked = blob;   // Deep copy via GeometryBlob's copy constructor.
        forked.refCount = referencesFromThisComponent;
        forked.dirty = true;
        remapBlobMaterials(forked, slotRemap);
        int32_t forkedIndex = poolInsertBlob(scene, std::move(forked));

        for (Chunk& chunk : scene.chunks) {
            if (!chunk.alive || chunk.geometryPoolIndex != poolIndex) continue;
            if (componentOfChunk(scene, chunk) != componentHandle) continue;
            chunk.geometryPoolIndex = forkedIndex;
            releaseBlob(scene, poolIndex);   // Drops this chunk's reference to the original.
        }
    }

    {
        std::lock_guard<std::mutex> lock(scene.materialPaletteMutex);
        ComponentRecord& comp = scene.components[componentHandle];
        comp.materialPalette.erase(comp.materialPalette.begin() + slot);
        comp.paletteVersion++;
    }

    core::info("removeMaterial: removed slot {} from component {} ({} voxel(s) reassigned to slot {}).",
               slot, componentHandle, usage[slot], slotIsUsed ? int(remappedReplacement) : -1);
    return true;
}

void brickMapSetMaterial(BrickData& brick, uint32_t localZOrder, uint8_t materialID) {
    brick.materials[localZOrder] = materialID;
}

uint8_t brickMapGetMaterial(const BrickData& brick, uint32_t localZOrder) {
    auto it = brick.materials.find(localZOrder);
    return (it != brick.materials.end()) ? it->second : 0;
}

void bakeMaterialsFromBrickMap(std::vector<uint32_t>& geometry,
                                std::vector<uint8_t>& materialIDs,
                                const VoxelBrickMap& map) {
    materialIDs.clear();
    size_t estimatedVoxels = 0;
    for (uint32_t bz = 0; bz < map.totalBricks; ++bz) {
        if (!map.bricks[bz]) continue;
        for (uint32_t row = 0; row < BRICK_MASK_ROWS; ++row) {
            estimatedVoxels += static_cast<size_t>(
                __builtin_popcountll(map.bricks[bz]->mask[row]));
        }
    }
    materialIDs.reserve(estimatedVoxels);

    size_t nodeCount = geometry.size() / 3;
    int32_t cursorBrick = 0;
    int32_t cursorRow = -1;

    auto advanceCursor = [&]() -> bool {
        while (cursorBrick < static_cast<int32_t>(map.totalBricks)) {
            ++cursorRow;
            if (cursorRow >= static_cast<int32_t>(BRICK_MASK_ROWS)) {
                ++cursorBrick;
                if (cursorBrick < static_cast<int32_t>(map.totalBricks)) {
                    cursorRow = -1;
                    continue; // skip the mask check this pass; next ++cursorRow -> 0
                } else break;
            }
            if (map.bricks[cursorBrick] &&
                map.bricks[cursorBrick]->mask[cursorRow] != 0) {
                return true;
            }
        }
        return false;
    };

    for (size_t n = 0; n < nodeCount; ++n) {
        uint32_t ptrFlag = geometry[n * 3 + 2];
        bool isLeaf = (ptrFlag & 1u) != 0;
        if (!isLeaf) continue;

        uint32_t materialOffset = static_cast<uint32_t>(materialIDs.size());

        if (!advanceCursor()) {
            core::error("bakeMaterialsFromBrickMap: cursor exhausted before leaf nodes");
            break;
        }

        const BrickData& brick = *map.bricks[cursorBrick];
        uint64_t rowBits = brick.mask[cursorRow];

        size_t runStart = materialIDs.size();
        bool uniform = true;
        uint8_t firstID = 0;
        bool haveFirst = false;

        while (rowBits) {
            int leadingZeros = __builtin_clzll(rowBits);
            uint32_t localZOrder = static_cast<uint32_t>(cursorRow) * 64 + leadingZeros;

            auto mit = brick.materials.find(localZOrder);
            uint8_t matID = (mit != brick.materials.end()) ? mit->second : 0;
            materialIDs.push_back(matID);
            if (!haveFirst) { firstID = matID; haveFirst = true; }
            else if (matID != firstID) uniform = false;

            rowBits &= ~(1ull << (63 - leadingZeros));
        }

        // Uniform-leaf compression. Terrain-like content fills whole leaves with one material
        // (measured ~96% of leaves), so collapse those to a single byte and let the shader skip
        // the per-voxel index. Random access is preserved -- the shader just ignores `above`.
        if (uniform && materialIDs.size() > runStart + 1) {
            materialIDs.resize(runStart + 1);
        }

        geometry[n * 3 + 2] = encodeLeafNode(materialOffset, uniform);
    }
}

} // namespace projv::utils
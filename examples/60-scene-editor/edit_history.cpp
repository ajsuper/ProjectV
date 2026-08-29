#include <algorithm>

#include "edit_history.h"
#include "core/log.h"

namespace projv::editor {

    // =============================================================================
    // EditHistory
    // =============================================================================

    void EditHistory::record(EditRecord newRecord, double nowSeconds) {
        newRecord.recordedAtSeconds = nowSeconds;

        // Recording after an undo abandons the redo tail: the user has taken a different branch, and
        // keeping the old one would mean redoing edits that no longer follow from the current state.
        if (cursor + 1 < int(records.size())) {
            for (size_t i = size_t(cursor + 1); i < records.size(); i++) {
                totalMemoryCost -= records[i].memoryCost;
            }
            records.erase(records.begin() + (cursor + 1), records.end());
        }

        // A colour drag emits one of these per frame. Merging consecutive edits to the same target
        // keeps the log readable and makes one drag one undo step — the undo of the *first* is what
        // survives, since that is the value the drag started from.
        if (!newRecord.coalesceKey.empty() && cursor >= 0) {
            EditRecord& previous = records[cursor];
            if (previous.coalesceKey == newRecord.coalesceKey &&
                nowSeconds - previous.recordedAtSeconds <= COALESCE_WINDOW_SECONDS) {
                previous.redo = std::move(newRecord.redo);
                previous.label = std::move(newRecord.label);
                previous.recordedAtSeconds = nowSeconds;
                return;
            }
        }

        totalMemoryCost += newRecord.memoryCost;
        records.push_back(std::move(newRecord));
        cursor = int(records.size()) - 1;
        evictOldestIfOverBudget();
    }

    void EditHistory::evictOldestIfOverBudget() {
        // Drops from the far end, which is the oldest reachable undo step. The cursor moves with it
        // so it keeps pointing at the same entry.
        while (totalMemoryCost > MEMORY_BUDGET && records.size() > 1) {
            totalMemoryCost -= records.front().memoryCost;
            records.erase(records.begin());
            cursor--;
            core::warn("Edit history over its memory budget: dropped the oldest undo step.");
        }
    }

    bool EditHistory::canUndo() const { return cursor >= 0; }
    bool EditHistory::canRedo() const { return cursor + 1 < int(records.size()); }

    bool EditHistory::undo() {
        if (!canUndo()) return false;
        records[cursor].undo();
        cursor--;
        return true;
    }

    bool EditHistory::redo() {
        if (!canRedo()) return false;
        records[cursor + 1].redo();
        cursor++;
        return true;
    }

    void EditHistory::jumpTo(int index) {
        index = std::clamp(index, -1, int(records.size()) - 1);
        while (cursor > index && undo()) {}
        while (cursor < index && redo()) {}
    }

    void EditHistory::clear() {
        records.clear();
        cursor = -1;
        totalMemoryCost = 0;
    }

    // =============================================================================
    // Palette snapshots
    // =============================================================================

    size_t PaletteSnapshot::memoryCost() const {
        size_t cost = palette.size() * sizeof(Material);
        for (const std::pair<int32_t, std::vector<uint8_t>>& blob : blobMaterials) {
            cost += blob.second.size();
        }
        cost += chunkPoolIndices.size() * sizeof(std::pair<ChunkHandle, int32_t>);
        for (const auto& brick : brickMaterials) {
            cost += brick.second.size() * sizeof(std::pair<uint32_t, uint8_t>);
        }
        return cost;
    }

    namespace {
        // Same rule the engine uses: a grid-resident chunk belongs to its grid's component, a loose
        // one carries its own.
        ComponentHandle componentOfChunk(const Scene& scene, const Chunk& chunk) {
            if (chunk.gridIndex >= 0 && size_t(chunk.gridIndex) < scene.grids.size()) {
                return scene.grids[chunk.gridIndex].componentHandle;
            }
            return chunk.componentHandle;
        }
    }

    PaletteSnapshot capturePaletteSnapshot(const Scene& scene, ComponentHandle componentHandle,
                                           bool includeGeometry) {
        PaletteSnapshot snapshot;
        if (componentHandle >= scene.components.size()) return snapshot;

        const ComponentRecord& component = scene.components[componentHandle];
        snapshot.component = componentHandle;
        snapshot.palette = component.materialPalette;
        snapshot.paletteVersion = component.paletteVersion;
        if (!includeGeometry) return snapshot;

        std::vector<int32_t> capturedBlobs;
        for (ChunkHandle handle = 0; handle < scene.chunks.size(); handle++) {
            const Chunk& chunk = scene.chunks[handle];
            if (!chunk.alive || chunk.geometryPoolIndex < 0) continue;
            if (componentOfChunk(scene, chunk) != componentHandle) continue;

            snapshot.chunkPoolIndices.push_back({handle, chunk.geometryPoolIndex});
            if (std::find(capturedBlobs.begin(), capturedBlobs.end(), chunk.geometryPoolIndex) != capturedBlobs.end()) {
                continue;   // Shared geometry is captured once, not once per instance.
            }
            capturedBlobs.push_back(chunk.geometryPoolIndex);

            const GeometryBlob& blob = scene.geometryPool[chunk.geometryPoolIndex];
            snapshot.blobMaterials.push_back({chunk.geometryPoolIndex, blob.materialIDs});

            if (!blob.brickMap) continue;
            // Flattened: the brick map is a sparse array of hash maps, and the snapshot only needs
            // the values, keyed by brick index and Z-order within it.
            std::vector<std::pair<uint32_t, uint8_t>> brickValues;
            for (size_t brickIndex = 0; brickIndex < blob.brickMap->bricks.size(); brickIndex++) {
                const std::unique_ptr<BrickData>& brick = blob.brickMap->bricks[brickIndex];
                if (!brick) continue;
                for (const std::pair<const uint32_t, uint8_t>& voxel : brick->materials) {
                    brickValues.push_back({uint32_t(brickIndex), voxel.second});
                }
            }
            snapshot.brickMaterials.push_back({chunk.geometryPoolIndex, std::move(brickValues)});
        }
        return snapshot;
    }

    void restorePaletteSnapshot(Scene& scene, const PaletteSnapshot& snapshot) {
        if (snapshot.component >= scene.components.size()) return;

        {
            std::lock_guard<std::mutex> lock(scene.materialPaletteMutex);
            ComponentRecord& component = scene.components[snapshot.component];
            component.materialPalette = snapshot.palette;
            // Forward, never back: the version is a watermark the GPU mirror compares against, so
            // restoring the old number would leave the mirror believing it is already current.
            component.paletteVersion++;
        }

        // Chunks first: a removal can have forked shared geometry and repointed them at the copy,
        // and the material bytes below belong to the blobs they pointed at originally.
        for (const std::pair<ChunkHandle, int32_t>& entry : snapshot.chunkPoolIndices) {
            if (entry.first >= scene.chunks.size()) continue;
            Chunk& chunk = scene.chunks[entry.first];
            if (chunk.geometryPoolIndex == entry.second) continue;
            if (entry.second < 0 || size_t(entry.second) >= scene.geometryPool.size()) continue;

            releaseBlob(scene, chunk.geometryPoolIndex);   // Let go of the fork.
            chunk.geometryPoolIndex = entry.second;
            scene.geometryPool[entry.second].refCount++;
            scene.geometryPool[entry.second].dirty = true;
        }

        for (const std::pair<int32_t, std::vector<uint8_t>>& entry : snapshot.blobMaterials) {
            if (entry.first < 0 || size_t(entry.first) >= scene.geometryPool.size()) continue;
            GeometryBlob& blob = scene.geometryPool[entry.first];
            blob.materialIDs = entry.second;
            blob.dirty = true;
        }

        for (const auto& entry : snapshot.brickMaterials) {
            if (entry.first < 0 || size_t(entry.first) >= scene.geometryPool.size()) continue;
            GeometryBlob& blob = scene.geometryPool[entry.first];
            if (!blob.brickMap) continue;
            size_t valueIndex = 0;
            for (size_t brickIndex = 0; brickIndex < blob.brickMap->bricks.size(); brickIndex++) {
                std::unique_ptr<BrickData>& brick = blob.brickMap->bricks[brickIndex];
                if (!brick) continue;
                for (std::pair<const uint32_t, uint8_t>& voxel : brick->materials) {
                    if (valueIndex >= entry.second.size()) break;
                    voxel.second = entry.second[valueIndex++].second;
                }
            }
        }
    }
}

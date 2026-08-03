#ifndef PROJV_EDITOR_EDIT_HISTORY_H
#define PROJV_EDITOR_EDIT_HISTORY_H

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "data_structures/scene.h"

// =============================================================================
// edit_history — the editor's undo log.
//
// Every edit the user makes is recorded as a pair of closures: one that puts the scene back the way
// it was, one that applies the change again. That is deliberately more general than a list of
// "palette recolour" structs, because the operations coming next (voxel add, voxel erase, transform
// moves) undo in completely different ways, and each of them knows how to reverse itself far better
// than a central switch statement would.
//
// The log is linear: recording a new edit after undoing discards the redo tail, which is what every
// editor does and what users expect.
// =============================================================================

namespace projv::editor {

    // One reversible edit.
    struct EditRecord {
        std::string label;                  // Shown in the Edit menu and the History panel.
        std::function<void()> undo;
        std::function<void()> redo;
        // Roughly how much heap the record's captured state holds. The log evicts its oldest entries
        // when the total gets out of hand — an undo step that snapshots material bytes for a large
        // component can be megabytes, and an unbounded history of those is a leak with a nice name.
        size_t memoryCost = 0;
        // Consecutive edits sharing a key merge into one entry (a colour drag is one undo step, not
        // one per frame). Empty means "never merge".
        std::string coalesceKey;
        double recordedAtSeconds = 0.0;
    };

    class EditHistory {
        public:
            // Records an edit that has *already been applied*. Merges into the previous entry when
            // the coalesce keys match and the two happened within a short window of each other.
            void record(EditRecord record, double nowSeconds);

            bool canUndo() const;
            bool canRedo() const;

            // Reverses / reapplies one edit. Returns false when there is nothing to do.
            bool undo();
            bool redo();

            // Steps to the state just after entry `index` (-1 = before every entry), undoing or
            // redoing as many times as it takes. This is what clicking an entry in the History panel
            // does.
            void jumpTo(int index);

            void clear();

            const std::vector<EditRecord>& entries() const { return records; }
            // Index of the entry that will be undone next; -1 when everything is undone.
            int position() const { return cursor; }
            size_t memoryUsed() const { return totalMemoryCost; }

        private:
            void evictOldestIfOverBudget();

            std::vector<EditRecord> records;
            int cursor = -1;                 // Index of the last applied entry.
            size_t totalMemoryCost = 0;
            // Generous, but bounded: undoing a palette removal on a large component snapshots its
            // material bytes, and those run to megabytes each.
            static constexpr size_t MEMORY_BUDGET = 512ull * 1024ull * 1024ull;
            static constexpr double COALESCE_WINDOW_SECONDS = 1.0;
    };

    // =============================================================================
    // Palette snapshots
    // =============================================================================
    //
    // Removing a palette entry renumbers every slot above it and rewrites the material byte of every
    // voxel that referenced them — and, where the geometry was shared with another component, forks
    // the blob and repoints chunks at the copy. None of that is recoverable from the palette alone,
    // so the undo step for a removal is a snapshot taken before the fact.

    struct PaletteSnapshot {
        ComponentHandle component = INVALID_COMPONENT_HANDLE;
        std::vector<Material> palette;
        uint32_t paletteVersion = 0;

        // Per-blob material bytes, and which blob each of the component's chunks pointed at. Both are
        // needed: a removal can change either.
        std::vector<std::pair<int32_t, std::vector<uint8_t>>> blobMaterials;
        std::vector<std::pair<ChunkHandle, int32_t>> chunkPoolIndices;
        // Brick-map materials, present only once a component has been voxel-edited. Same reason as
        // the baked bytes: they are the source the blob is rebuilt from.
        std::vector<std::pair<int32_t, std::vector<std::pair<uint32_t, uint8_t>>>> brickMaterials;

        size_t memoryCost() const;
    };

    /**
     * Captures everything a palette edit could destroy.
     * @param scene The scene to read.
     * @param componentHandle The component whose palette is about to change.
     * @param includeGeometry True for edits that rewrite voxel material bytes (removals). False for
     *                        edits that only touch the palette, which keeps the snapshot tiny.
     */
    PaletteSnapshot capturePaletteSnapshot(const Scene& scene, ComponentHandle componentHandle,
                                           bool includeGeometry);

    /**
     * Puts a captured snapshot back, marking whatever it touched dirty so the next
     * graphics::flushSceneUpdates re-uploads it.
     */
    void restorePaletteSnapshot(Scene& scene, const PaletteSnapshot& snapshot);
}

#endif

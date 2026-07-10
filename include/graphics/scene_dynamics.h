#ifndef PROJV_SCENE_DYNAMICS_H
#define PROJV_SCENE_DYNAMICS_H

#include <vector>

#include "data_structures/scene.h"
#include "data_structures/gpuData.h"

// The apply-mutation seam: the single, policy-free point where in-memory scene changes (streaming
// load/evict, live edits) are reflected onto the GPU. Producers enqueue PendingSceneMutations against a
// plain std::vector; a per-frame applySceneMutations drains them through the incremental primitives and
// syncs the small scene tables ONCE (instead of once per mutation). This is the mechanism both the
// streaming residency policy and the future editing UI feed. See compose_data_structure.md.
namespace projv::graphics {
    enum class SceneMutationKind {
        Add,    // A chunk became resident: upload its blob (if new) + header row, register membership.
        Update, // A chunk's pool blob geometry changed (an edit): re-fit its GPU range + rewrite header(s).
        Remove  // A chunk was evicted/removed: free its range at refcount 0, recycle slot, clear membership.
    };

    struct PendingSceneMutation {
        SceneMutationKind kind;
        ChunkHandle handle;
    };

    /**
     * Drains `queue`, applying each mutation with the incremental primitives but DEFERRING the
     * gridInfo/cellMap/looseList rebuild to a single sync at the end — so a burst of streaming
     * loads/evicts rebuilds those small tables once per frame, not once per chunk. Clears the queue.
     * Headless-safe (the primitives no-op their bgfx uploads when textures are invalid).
     * @param scene The scene owning the chunks and geometry pool.
     * @param gpuData The GPU data/allocation table to mutate.
     * @param queue The pending mutations to apply; emptied on return.
     */
    void applySceneMutations(projv::Scene& scene, GPUData& gpuData, std::vector<PendingSceneMutation>& queue);
}

#endif

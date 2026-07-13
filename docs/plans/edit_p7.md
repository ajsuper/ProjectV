# Phase 7 — EditingPolicy + Selective Editing + SceneDiff

## Goal

Move mutability out of the `Scene` data structures into an `EditingPolicy`
that lives at the API/interface layer. Add per-component drain
(`updateComponent`) and structured diff output (`SceneDiff`) from the editing
API. Add headless GPU guards.

This depends on P6: the hierarchical component tree must be in place so
`EditingPolicy` can be indexed by `ComponentHandle`.

## What exists (before P7)

- `enum class Mutability` in `scene.h` with values `Locked`, `Direct`, `Copy`.
- `Chunk::mutability` field set by loader, never consulted by editing code.
- `updateScene(Scene&)` drains ALL components — no way to drain one.
- No structured return value — callers must diff state themselves.
- `flushSceneUpdates` calls `bgfx::getCaps()` unconditionally — can't run
  headless.

## What's missing

1. `Mutability` should not live on `Scene`. Must move to API layer.
2. `EditingPolicy` — per-component mutability, session flags.
3. `EditingContext` — bundles `Scene&` + `EditingPolicy` for clean API.
4. Policy-driven COW: Locked/Direct = edit in-place, Copy = fork.
5. `updateComponent(Scene&, ComponentHandle)` — drain one component.
6. `SceneDiff` — structured return value describing what changed.
7. Headless guard — no bgfx symbols referenced in headless paths.

## Sub-phases

### P7.1 — Move Mutability out of Scene (editing.h, scene.h)

**Files:** `include/data_structures/scene.h`, `include/utils/editing.h`,
`include/data_structures/compose.h`

**P7.1a — Remove `Mutability` from `scene.h`**

Delete the `enum class Mutability` block from `scene.h`. Move it to
`editing.h`.

**P7.1b — Remove `Chunk::mutability`**

Delete the `Mutability mutability = Mutability::Locked;` field from `Chunk`
in `scene.h`. No replacement — the policy lives in `EditingPolicy` now.

**P7.1c — Update `compose.h`**

`ComposeComponent` still needs a `Mutability` field (it's read from
compose.json). Change its type from `projv::Mutability` to reference the
moved enum. Since `Mutability` is now in `editing.h`, change the include:

```cpp
// compose.h — was: #include "data_structures/scene.h" // for projv::Mutability
// Now: just forward-declare or include editing.h
//     Actually, editing.h includes scene.h already, so:
#include "utils/editing.h"   // for projv::Mutability
```

The `ComposeComponent::mutability` field stays — it's the on-disk policy
that gets loaded into `EditingPolicy`.

**P7.1d — Update `compose_io.cpp`**

Remove the line `chunk.mutability = c.mutability;` from the loader.
Instead, the loader will fill `EditingPolicy` (see P7.2b).

**P7.1e — Update all references to `Mutability` and `Chunk::mutability`**

Search for `chunk.mutability` and `Chunk::mutability` everywhere. Remove
or replace with `EditingPolicy::get(...)`. Files to check:
- `src/utils/compose_io.cpp` — the `makeChunk` lambda (remove the one assignment)
- All examples (`editing_p1`, `edit_demo`) — check for mutability usage
- `gpu_interface.cpp` — should not touch mutability (it never did)

### P7.2 — EditingPolicy + EditingContext (editing.h, editing.cpp)

**Files:** `include/utils/editing.h`, `src/utils/editing.cpp`

Add the new types:

```cpp
namespace projv::utils {

enum class Mutability { Locked, Direct, Copy };

struct EditingPolicy {
    // Per-component mutability, indexed by ComponentHandle.
    // Empty = all components default to Copy.
    std::vector<Mutability> componentPolicies;

    // Optional runtime lock (editor checkbox, server authority).
    // Independent of compose-spec mutability. Overrides to "reject all edits."
    std::vector<bool> runtimeLocked;

    // If true, Locked components reject edits at queue time.
    bool rejectLockedEdits = false;

    // Global override: treat every component as if it were this mutability.
    // nullopt = use per-component policies.
    std::optional<Mutability> globalOverride;

    Mutability get(ComponentHandle h) const;
    bool isLocked(ComponentHandle h) const;
};

struct EditingContext {
    Scene& scene;
    EditingPolicy policy;

    // P8: edit groups, transaction state, discard boundary
};

}
```

**P7.2a — EditingPolicy::get implementation**

```cpp
Mutability EditingPolicy::get(ComponentHandle h) const {
    if (globalOverride.has_value()) return globalOverride.value();
    if (h < componentPolicies.size()) return componentPolicies[h];
    return Mutability::Copy;   // default
}

bool EditingPolicy::isLocked(ComponentHandle h) const {
    if (h < runtimeLocked.size() && runtimeLocked[h]) return true;
    if (rejectLockedEdits && get(h) == Mutability::Locked) return true;
    return false;
}
```

### P7.3 — Policy-driven COW (editing.cpp)

**Files:** `src/utils/editing.cpp`

Modify `applyEditsToChunk` and the Chunk/Grid paths in `applyComponentQueue`
to consult the editing policy.

**P7.3a — API signature change**

Change `applyComponentQueue` to take an `EditingContext&` instead of plain
`Scene&`:

```cpp
// Before:
bool applyComponentQueue(Scene& scene, ComponentHandle h);

// After:
bool applyComponentQueue(EditingContext& ctx, ComponentHandle h);
```

Where `ctx.scene` replaces the old `scene` parameter.

**P7.3b — Locked check at drain time**

At the top of `applyComponentQueue`, check whether the component is locked:

```cpp
ComponentRecord& comp = ctx.scene.components[h];
if (ctx.policy.isLocked(h)) {
    if (ctx.policy.rejectLockedEdits) {
        comp.editQueue.ops.clear();
        return false;   // rejected
    }
    // Non-reject Locked: allow edit in-place (see below)
}
```

**P7.3c — Fork vs in-place decision**

Replace the unconditional `forkBlob` call with a policy-conditional one:

```cpp
// In the Chunk path, instead of:
int32_t newIdx = forkBlob(scene, oldIdx);

// Do:
Mutability mut = ctx.policy.get(h);
int32_t newIdx = oldIdx;
if (mut == Mutability::Copy) {
    newIdx = forkBlob(ctx.scene, oldIdx);
} else {
    // Locked or Direct: edit in-place (set dirty, no fork)
    if (oldIdx >= 0) {
        ctx.scene.geometryPool[oldIdx].dirty = true;
    }
}
chunk.geometryPoolIndex = newIdx;
```

For the Grid path, the same logic applies to each cell's chunk. Replace
calls to `forkBlob` inside the Grid's per-cell loop with the same
policy-driven pattern.

For new cells (grid expansion), no fork is needed — `internChunkGeometry`
already creates a fresh blob. No change needed there.

**P7.3d — Clear edit queue on Locked rejection**

In the Locked rejection path at the top, clear the queue and return:

```cpp
if (ctx.policy.rejectLockedEdits && ctx.policy.isLocked(h)) {
    comp.editQueue.ops.clear();
    core::warn("[EDIT] Component {} is Locked — {} ops rejected",
               h, comp.editQueue.ops.size());
    return false;
}
```

### P7.4 — Per-component drain (editing.h, editing.cpp)

**Files:** `include/utils/editing.h`, `src/utils/editing.cpp`

**P7.4a — Add public `updateComponent`**

```cpp
/**
 * Drain a single component's edit queue. Like updateScene but only processes
 * one component. Returns the SceneDiff for this one component.
 * @param ctx The editing context (scene + policy).
 * @param h The component to process.
 * @return SceneDiff describing what changed (empty if nothing).
 */
SceneDiff updateComponent(EditingContext& ctx, ComponentHandle h);
```

Implementation:

```cpp
SceneDiff updateComponent(EditingContext& ctx, ComponentHandle h) {
    SceneDiff diff;
    if (h >= ctx.scene.components.size()) return diff;
    if (ctx.scene.components[h].editQueue.ops.empty()) return diff;
    if (applyComponentQueue(ctx, h)) {
        // SceneDiff is accumulated inside applyComponentQueue
        diff = std::move(ctx.scene.components[h].editDiff);  // see P7.5
    }
    return diff;
}
```

Note: the `SceneDiff` accumulation happens in `applyComponentQueue` (P7.5).
We store a temporary `SceneDiff editDiff` on `ComponentRecord` (or return it
from `applyComponentQueue`).

**P7.4b — Refactor `updateScene`**

```cpp
SceneDiff updateScene(EditingContext& ctx) {
    SceneDiff totalDiff;
    for (ComponentHandle h = 0; h < ctx.scene.components.size(); ++h) {
        if (ctx.scene.components[h].editQueue.ops.empty()) continue;
        if (ctx.scene.components[h].kind == ComponentKind::Asset) continue;
        SceneDiff part = updateComponent(ctx, h);
        totalDiff.merge(part);
    }
    return totalDiff;
}
```

Keep the old `updateScene(Scene&)` signature as a convenience overload that
uses a default `EditingPolicy` (all Copy):

```cpp
// Convenience overload: default policy
uint32_t updateScene(Scene& scene) {
    EditingContext ctx{scene, EditingPolicy{}};
    return updateScene(ctx).modifiedChunks.size();  // approximate count
}
```

This keeps backward compatibility with the P1-P5 signatures.

### P7.5 — SceneDiff (scene.h, editing.cpp)

**Files:** `include/data_structures/scene.h` (or a new header),
`src/utils/editing.cpp`

**P7.5a — Define SceneDiff**

```cpp
struct ComponentChunkEdit {
    ComponentHandle componentHandle;
    int32_t         oldPoolIndex;   // -1 if new chunk was created
    int32_t         newPoolIndex;   // -1 if blob was freed
    ChunkHandle     chunkHandle;
};

struct SceneDiff {
    std::vector<ComponentChunkEdit> chunkEdits;
    std::vector<ComponentHandle>    modifiedComponents;

    void merge(const SceneDiff& other);
    bool empty() const { return chunkEdits.empty() && modifiedComponents.empty(); }
};
```

Index pairs only — no content capture. Callers who need undo save blob
contents themselves.

`SceneDiff::merge` appends the other diff's vectors:

```cpp
void SceneDiff::merge(const SceneDiff& other) {
    chunkEdits.insert(chunkEdits.end(),
                      other.chunkEdits.begin(), other.chunkEdits.end());
    modifiedComponents.insert(modifiedComponents.end(),
                               other.modifiedComponents.begin(),
                               other.modifiedComponents.end());
}
```

**P7.5b — Accumulate SceneDiff in applyComponentQueue**

In the Chunk path, before and after fork/edit:

```cpp
ComponentChunkEdit edit;
edit.componentHandle = h;
edit.chunkHandle = comp.chunkHandle;
edit.oldPoolIndex = chunk.geometryPoolIndex;

// ... do the fork or in-place edit ...

edit.newPoolIndex = chunk.geometryPoolIndex;
// Only add if something actually changed
if (edit.oldPoolIndex != edit.newPoolIndex ||
    /* content changed (dirty flag set) */) {
    diff.chunkEdits.push_back(edit);
    diff.modifiedComponents.push_back(h);
}
```

For the Grid path, collect one `ComponentChunkEdit` per affected cell.

For new cell creation:

```cpp
ComponentChunkEdit edit;
edit.componentHandle = h;
edit.chunkHandle = newHandle;
edit.oldPoolIndex = -1;          // didn't exist before
edit.newPoolIndex = newChunkPoolIdx;
diff.chunkEdits.push_back(edit);
```

Return the accumulated `SceneDiff` from `applyComponentQueue` (or store it
on the component).

### P7.6 — Headless GPU safety (gpu_interface.cpp, gpu_interface.h)

**Files:** `src/graphics/gpu_interface.cpp`, `include/graphics/gpu_interface.h`

**P7.6a — Add s_hasGPU flag**

```cpp
// In gpu_interface.cpp, anonymous namespace
static bool s_hasGPU = false;

// Set in createTexturesForScene (only callable with live bgfx)
GPUData createTexturesForScene(projv::Scene& scene) {
    s_hasGPU = bgfx::isValid(bgfx::getStats());  // or similar trivial bgfx call
    // ... rest unchanged ...
}
```

**P7.6b — Guard bgfx::getCaps() calls**

Every function that calls `bgfx::getCaps()` should check first:

```cpp
uint32_t maxTexSize() {
    if (!s_hasGPU) return 16384u;  // reasonable fallback
    const bgfx::Caps* caps = bgfx::getCaps();
    // ... rest ...
}
```

Functions to guard: `maxTexSize`, `createArbitraryTexture`,
`createArbitraryTextureRGB`, `createCellMapTexture`, `createUintRowTexture`.

**P7.6c — No-op flushSceneUpdates for headless**

```cpp
void flushSceneUpdates(projv::Scene& scene, GPUData& gpuData) {
    if (!s_hasGPU) {
        // Clear dirty flags anyway — headless callers expect the state
        // to be "synced" even if there's no GPU.
        for (auto& blob : scene.geometryPool) blob.dirty = false;
        return;
    }
    // ... existing implementation ...
}
```

**P7.6d — Headless createTexturesForScene stub**

When called without a live bgfx context, return a degenerate GPUData:

```cpp
GPUData createTexturesForScene(projv::Scene& scene) {
    if (!bgfx::isValid(bgfx::getStats())) {
        // Headless: just intern unpooled chunks, return empty GPUData
        for (Chunk& c : scene.chunks)
            if (c.alive && c.geometryPoolIndex < 0) internChunkGeometry(scene, c);
        s_hasGPU = false;
        return GPUData{};  // all handles BGFX_INVALID_HANDLE
    }
    s_hasGPU = true;
    // ... existing implementation ...
}
```

## File change summary

| File | Change |
|------|--------|
| `include/data_structures/scene.h` | Remove `enum class Mutability`. Remove `Chunk::mutability`. Add `SceneDiff` struct. |
| `include/utils/editing.h` | Add `Mutability` enum. Add `EditingPolicy`, `EditingContext` structs. Declare `updateComponent`, `SceneDiff updateScene(ctx)`. |
| `src/utils/editing.cpp` | Implement `EditingPolicy::get/isLocked`. Implement policy-driven COW. Implement `updateComponent`. Implement `SceneDiff` accumulation. |
| `include/data_structures/compose.h` | Update `Mutability` reference (now from editing.h). No structural change. |
| `src/utils/compose_io.cpp` | Remove `chunk.mutability = c.mutability`. Loader fills `EditingPolicy` instead. |
| `include/graphics/gpu_interface.h` | No API change. |
| `src/graphics/gpu_interface.cpp` | Add `s_hasGPU` flag. Guard bgfx calls. No-op flush for headless. |
| `docs/examples/editing_p1/main.cpp` | Test `updateComponent` + `SceneDiff`. |
| `docs/examples/edit_demo/main.cpp` | Switch to `EditingContext` API. |

## Definition of done

- `Mutability` is no longer in `scene.h`. It lives in `editing.h`.
- `Chunk` has no `mutability` field.
- `EditingPolicy::get(h)` returns the correct policy, defaulting to `Copy`.
- `Locked` components with `rejectLockedEdits=true` reject edits (queue
  cleared, `updateComponent` returns empty diff).
- `Direct` components edit blobs in-place (no fork, dirty flag set, same
  pool index).
- `Copy` components fork blobs (existing behavior, unchanged).
- `updateComponent(ctx, h)` drains ONE component. Measurably faster than
  `updateScene` in a scene with N components and 1 dirty.
- `SceneDiff` contains correct `oldPoolIndex`/`newPoolIndex` pairs for every
  affected chunk.
- `flushSceneUpdates` and `createTexturesForScene` do not call bgfx symbols
  when called without a live GPU context.
- All P1-P6 assertions still pass.
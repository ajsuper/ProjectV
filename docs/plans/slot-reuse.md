# Slot reuse: generational components, recycled chunks, indexed roots

Status: approved 2026-10-09; steps 1, 2, 3 (engine and runtime) and 5 done. Open: the editor conversion (step 4).

## Why

Measured in examples/16-sandbox with 150 live bodies and 50 spawn/destroy pairs a frame:

| after | component rows | chunk rows | header texture rows | palette entries | CPU frame |
|---|---|---|---|---|---|
| 5k spawns | 10,103 | 5,310 | 5,914 | | 6.2 ms |
| 60k spawns | 120,103 | 60,310 | 89,914 | 180,161 | 28.2 ms |

GPU time stayed flat (4.6 ms). The CPU growth is spawn: 0.76 ms a spawn at 120k rows, almost all
of it `nextLocalId` for a new root, which walks every row ever made (`forEachSibling` with no
parent), tombstones included. Deleted rows were never reused, so everything indexed by row grew
without bound: the header texture, the palette (a dead component kept its palette), and every scan.

Physics makes this the common case, not the stress test: debris, fragments, projectiles.

## What changes

1. **Chunk slots are recycled.** `Scene::chunkFreeList`, like `blobFreeList`. `deleteComponent`
   frees a dead component's chunks (and a grid's cells) onto it, and every engine creation site
   allocates from it (`allocateChunkSlot`). A reused row is marked `headerDirty`, so the GPU rewrites
   its header. The tombstone's `chunkHandle` is cleared so nothing reaches the new occupant through it.

2. **Roots are indexed.** `utils::rootComponents(scene)` is the top-level components in creation
   order, kept by the engine's own create / reparent / delete, and catching up on rows a program
   appended to `scene.components` directly (examples and tests do). `forEachSibling(INVALID)` — so
   `nextLocalId`, `findComponentByLocalId`, `ensureUniqueLocalId` — walks it, not every row.

3. **Component slots are generational.** `ComponentRecord::generation` is bumped each time the slot
   is freed. `ComponentRef {handle, generation}` is what a long-lived holder keeps;
   `utils::refOf(scene, h)` makes one and `utils::resolve(scene, ref)` returns the handle while it is
   still the same component, `INVALID_COMPONENT_HANDLE` once it is not. Freed slots go on
   `Scene::componentFreeList` and `addComponent` / `duplicateComponent` / `instantiateComposeInto`
   reuse them. A freed record is emptied (palette, children, queues, attachments, paths) so a dead
   row costs almost nothing while it waits.

   Within a frame a bare `ComponentHandle` is still the way to name a component; scene-internal
   links (parent, children, `Chunk::componentHandle`) are kept exact by the scene itself.

4. **Rollout: `Scene::recycleSlots`.** On by default. The scene editor turns it off until its own
   long-lived handles are converted: ~25 fields (selection, stroke targets, the open asset, row sets
   and maps keyed by handle) and its undo history, which keeps both component and chunk handles
   (`edit_history.h`). With it off, deletion behaves exactly as before (tombstones, no reuse), so the
   editor cannot alias. Converting the editor to `ComponentRef` and turning it on is the follow-up.

5. **Spawn path.** A dead component's palette is cleared, so the palette texture holds live
   components only, and the palette rebuild notices slot changes (`Scene::slotEpoch`), not only
   palette versions — a reused slot can carry the same version sum the old one did. Prefab loads are
   cached (`runtime::PrefabCache` in `world.ctx()`): a folder is read once, and later spawns graft
   the cached copy. Loader chatter on the spawn path drops to trace.

## Holders converted in the engine

- `runtime::VoxelComponent` keeps the generation it linked; a link whose component is gone or reused
  unlinks rather than driving the new occupant.
- Attachments are cleared on delete already; nothing else in `utils/` keeps a handle across calls.
- `instantiateComposeInto` no longer assumes incoming rows land contiguously: it remaps through a
  table, because reused slots are scattered.

## Verification

- Unit tests: reuse after delete; a stale `ComponentRef` resolves to invalid; chunk rows and the
  header count stop growing under churn; roots index matches a brute-force scan after every kind of
  mutation, including direct appends; grafting into a scene with holes.
- Sandbox churn (`SANDBOX_CHURN`): rows, headers and palette entries flat; CPU frame flat.
- Sandbox self-test, 15-entities, editor self-tests (recycling off) unchanged.

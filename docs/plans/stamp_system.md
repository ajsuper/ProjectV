# Stamp System — Shape Placement and Region Copy/Move (SceneEditor)

**Superseded** by [`assembly_system.md`](assembly_system.md), which kept every mechanism this
document built -- the pull-not-push rasterisation, the lattice snapping, the budget discipline, the
Extrude-shaped undo record, the region selectors -- and changed what they are arranged around. A
stamp is a part of a persistent assembly now, and the merge is a fold. Read this for why the
mechanisms are the way they are; read that one for what the tool does.

**Implemented.** This is the design it was built from; the built version is documented in
[`docs/examples/SceneEditor/README.md`](../examples/SceneEditor/README.md) under *Stamps: the Shape
and Region tools*, and lives in that example's `main.cpp`. Where the two differ, the README is what
the code does.

Three deliberate departures from what is written below:

- **`Ctrl+Y` also cost Redo its alias.** The shortcut table here does not mention that the letter was
  already taken; Redo keeps `Ctrl+Shift+Z`, which the Edit menu had always advertised alongside it.
- **Stamps are a list, not a singleton.** The design's "at most one exists at a time" was built first
  and then replaced: several stamps float at once, shift+click adds to the selection, and Merge
  commits the selection as one history entry. Placing, lifting, cutting, cancelling and merging are
  all undoable, and undoing a merge brings the stamps back floating.
- **A merge past its cell budget is refused, not truncated.** "Budget discipline" below is borrowed
  from the fill, but a fill that stops halfway leaves a smaller fill, while a merge that stops
  halfway leaves half an object embedded in the scene. The message names the number and the fix.

And one hazard below was answered the wrong way round. Hazard 1 proposes reusing one stamp component
per session to dodge the soft-delete leak. That was tried and reverted: a reused component is a **live
node in the Scene Hierarchy that outlives the stamp it held**, so every placement left an empty
`Stamp` behind in the user's tree. A leftover node is a bug; an unreclaimed record is a known cost of
handles being indices. The built version creates a component per placement and deletes it on release,
and the `deleteComponent` helper the hazard also asks for is what makes that one line at three call
sites.

## Goal

Two features the SceneEditor is missing, which turn out to be one machine:

1. **Shape generator/placer** — spawn a primitive (box, sphere, cylinder, cone, wedge,
   pyramid, torus) into the scene, drag and rotate it into position, then merge it into
   the surrounding geometry.
2. **Region select** — select an area or set of voxels already in the scene, then copy or
   move them with the same gizmo and commit them somewhere else.

Both produce **a floating stamp**: geometry that is not part of the scene yet, carries its
own transform, is driven by the transform gizmo, and ends in a commit or a cancel. Build
the stamp once and the two features become two *sources* feeding it.

## The central decision: a stamp is a real component

A floating stamp is a live `ComponentKind::Chunk` component in `Scene::components`, not a
CPU-side voxel buffer with a preview overlay.

The buffer version needs a new render path and cannot be sculpted before it commits. The
component version inherits, at no cost:

- **rendering** — the raycaster already draws any component, and `ChunkHeader::rotation` is
  already honoured, so a rotated stamp is visible as it will land;
- **the transform gizmo** — `updateAndDrawTransformGizmo` already operates on
  `editor.selectedComponent`;
- **the yellow selection outline** (`collectLeafChunks`);
- **Sculpt and Paint working on the stamp before it commits**, which is the whole of "edit
  it how you please" for free;
- **"keep this as its own object"** as a no-op commit path.

## What exists (and is reused verbatim)

- `utils::addComponent(scene, Chunk, name, parent, resolution, voxelScale)` — creates the
  stamp. Resolution must be a power of **four**; `utils::isValidChunkResolution` checks it.
- `utils::queueVoxelAdd` / `queueVoxelRemove` → `utils::updateScene` →
  `graphics::flushSceneUpdates` — the write path every existing tool uses.
- `componentVoxelToWorld` / `worldToComponentVoxel` (`main.cpp`, `ComponentVoxelSpace`) —
  exact inverses, and the whole basis of a lossless merge.
- `gatherFaceRegion` — already gathers a coplanar, same-facing, 4-connected face, already
  under `SelectionScope`. This *is* the Region tool's "Face" selector.
- The volume fill's breadth-first traversal over a per-chunk bitset — this *is* the "Volume"
  selector, and its bitset is how a large selection is stored.
- `SelectionScope` (Material vs Everything) — inherited unchanged. On photo-textured scenes
  that toggle is the difference between selecting 4 voxels and 32,000.
- Extrude's displaced-cell records — the exact undo shape a merge needs (below).
- `editor::EditHistory` — closure pairs, one entry per gesture.

## The two tools

```cpp
enum class EditorTool {
    Select,   // Ctrl+Q
    Move,     // Ctrl+W
    Sculpt,   // Ctrl+E
    Paint,    // Ctrl+R
    Shape,    // Ctrl+T — generate a primitive and place it
    Region    // Ctrl+Y — select voxels, copy/cut them, place them
};
```

The shortcuts continue the keyboard-row logic already written down above `EditorTool`.

Two tools rather than one, because a tool is *what left-click means* — the justification
already in the file — and these answer differently. Shape spawns from a panel button and is
then all gizmo; Region needs a left-drag in the viewport to define the selection. Same
subsystem underneath, shared placement block in both panels.

| | Shape (Ctrl+T) | Region (Ctrl+Y) |
|---|---|---|
| Left-click | Spawns the primitive at the surface under the cursor, or `Place distance` down the ray when there is nothing there — the fallback Sculpt already has | Picks corner A, then corner B; or gathers a face/volume |
| Panel top | Shape list, size in voxels, hollow + wall thickness, `drawToolMaterialRow` | Selector (Box/Face/Volume) + `SelectionScope`, selection stats, Copy/Cut/Delete/Fill |
| Panel bottom | shared placement block | shared placement block |

Sizes are in **voxels** with the world size shown underneath, for the reason the Paint panel
already gives: a radius in world units means a different number of voxels per component.

## The stamp

```cpp
struct FloatingStamp {
    bool active = false;
    ComponentHandle component = INVALID_COMPONENT_HANDLE;  // live, renderable, gizmo-able
    ComponentHandle target    = INVALID_COMPONENT_HANDLE;  // where Merge writes
    enum class Source { Generated, Lifted } source;

    bool cutFromTarget = false;                  // Cancel must put the lifted voxels back
    std::vector<PendingVoxelOp> liftedOriginal;  // ...which is what this holds

    // Generated only. Regenerating a primitive at new dimensions is exact and free, which
    // is why generated shapes can be resized after spawning and lifted ones cannot.
    ShapeKind kind; int dimensions[3]; bool hollow; int wallThickness; uint8_t materialSlot;
};
```

**Creation** takes `voxelScale` from the *target* component, never from
`editor.createVoxelScale`. A stamp at a different voxel size makes every merge a resample.
`resolution` is the smallest power of four that fits the largest dimension. The primitive is
rasterised into one `queueVoxelAdd` + one `updateScene`.

**Manipulation.** Relax the gizmo's guard from `activeTool == Move` to
`activeTool == Move || stamp.active`. That is the only change the gizmo needs.

Keyboard, while a stamp is floating:

- arrows nudge **one voxel of the target lattice** in the camera-relative ground plane,
  PgUp/PgDn vertical;
- Ctrl+arrows rotate 90° about the most camera-facing axis;
- **Enter merges, Esc cancels.**

Translation always snaps to the target's voxel lattice.

## Rotation: one toggle, two honest modes

**`Snap 90°` — a single toggle button, on by default.**

**On.** Rotation is a multiple of 90° about a lattice axis, translation is an integer number
of voxels, and the voxel scales match — so the merge is a **1:1 integer remap**. Every source
voxel lands on exactly one target cell, nothing aliases, nothing is lost, and merging a
lifted region straight back where it came from is a byte-identical no-op. This is the mode
for everything built to the grid.

**Off.** Free rotation, and the merge **rasterises the rotated shape into the target
lattice**. This is not a degraded fallback — it is the point of the setting. A wedge or
pyramid meant to sit at 30° in the final geometry, or a tree placed at its own angle so a
dozen copies do not read as a dozen copies, can only be made this way. The UI should say so
plainly ("Free rotation — the merge rasterises into the target lattice") rather than warning
about it; a warning would be telling the user their deliberate choice is a mistake.

### The rasterisation must pull, not push

Forward-mapping each stamp voxel to a target cell (**push**) leaves holes. A rotation is not
area-preserving on a lattice, so two source voxels can land in one target cell while a
neighbouring cell receives none — and on a hollow shape, whose walls are one or two voxels
thick, those gaps perforate the surface. The result is a rotated shape you can see through.

So the merge iterates the **target** cells instead (**pull**): take the stamp's oriented
bounding box, walk every target cell inside it, inverse-transform that cell's centre into the
stamp's voxel space, and sample. Every target cell gets exactly one answer, so the surface is
closed by construction. Cost is the volume of the oriented bounding box rather than the voxel
count of the stamp — which is why the merge needs the fill's budget discipline, and why the
box's volume, not the stamp's voxel count, is the number to cap.

With `Snap 90°` on, the pull degenerates to the exact integer remap: the inverse transform
is a signed axis permutation, and every target cell in the box maps to exactly one source
cell. One code path serves both modes.

## Commit

```cpp
mergeStamp(scene, stamp, MergeMode::Add | MergeMode::Subtract);
```

Each merged voxel carries **the stamp voxel's own colour**, not the palette's current entry —
the same principle Extrude already follows in taking its source voxel's material. Everything
goes out in one queue call, one `updateScene`, and **one history entry**.

`Subtract` earns its place from day one: spawn a sphere, subtract, and you have a crater. It
is the same walk through `queueVoxelRemove`.

**The undo record is exactly Extrude's.** Displaced cells are split two ways — cells that
were empty (reverse by removing) and cells that held something (reverse by writing the old
contents back). The naive version — "adding fills empty space, so undo empties it again" — is
wrong the moment a stamp lands on geometry that was already there, and deletes voxels the
merge never created. That lesson is already paid for in this codebase; do not re-learn it.

Three exits, all buttons in the shared placement block:

- **Merge** — write into the target, release the stamp.
- **Keep as component** — clear the floating flag and leave it in the hierarchy as its own
  object. Often what is actually wanted for a placed pillar or tree.
- **Cancel** — release the stamp, and if it was a *cut*, put `liftedOriginal` back.

## Region selection

A selection belongs to one component, the way an edit queue does:

```cpp
struct VoxelSelection {
    ComponentHandle component;
    core::ivec3 boundsMin, boundsMax;
    std::vector<uint64_t> bits;   // one bit per cell of the bounds box
};
```

Bounds plus a bitset, not a `vector<ivec3>` — the volume fill already established why: a
million-voxel selection is 128 KB one way and 12 MB the other.

Three selectors, mirroring Paint's shape list:

- **Box** — click voxel A, click voxel B, live wireframe between them. No modifier
  gymnastics, unambiguous, and it is what a first version should ship.
- **Face** — `gatherFaceRegion`, unchanged.
- **Volume** — the fill traversal, returning its bitset instead of painting it.

Actions: **Copy** (lift, source untouched), **Cut** (lift *and* remove the source in the same
history entry), **Delete**, **Fill** with the current palette entry, **Duplicate in place**
(copy offset by one bounding box).

A lift builds a fresh Chunk at the target's `voxelScale`, resolution sized to the selection's
bounding box, positioned so its lattice **coincides with the source's** — so the stamp appears
exactly over the original with no visual jump — then queues the selection's voxels translated
by `-boundsMin`.

Once lifted it is the same `FloatingStamp` the Shape tool produces. Same gizmo, same snapping,
same three exits. That is the payoff of the shared design.

## Where the UI lives

- **Viewport tool strip** (`drawViewportToolbar`) and the Tool panel strip: two more entries.
  `TOOL_ICONS` / `EDITOR_TOOL_COUNT` are table-driven, so each tool is an enum entry, a label,
  a hint, a shortcut, and one icon function.
- **Tool panel**: `drawShapeToolSettings` / `drawRegionToolSettings`, mirroring
  `drawSculptToolSettings`. Both end with a shared `drawStampPlacement` — Target, `Snap 90°`,
  merge mode, and the three buttons.
- **Viewport banner** while a stamp is live: *"Placing: Sphere r=12 → Castle/walls · Enter
  merge · Esc cancel"*, anchored like the breadcrumb. A floating stamp is a modal state, and a
  mode the user cannot see they are in is the classic editor failure. It must survive a tool
  switch, so you can jump to Sculpt, tweak the stamp, and come back.

## Known hazards

1. **Component deletion is a soft delete.** The hierarchy's Delete unparents and renames to
   `__deleted__`; nothing reclaims the record. A create/destroy per placement leaks. Fix:
   **reuse one stamp component per session**, clearing and refilling its geometry, and only
   release it on "Keep as component". Also worth extracting that menu item's body into a real
   `deleteComponent` helper — three call sites will want it.
2. **`duplicateComponent` does not handle Grid components.** This design never calls it: a lift
   builds a fresh Chunk through `queueVoxelAdd`. Stated here so nobody "optimises" it later.
3. **Undo of an additive edit leaves empty Grid cells behind** — the known issue in the
   SceneEditor README. A merge inherits it exactly.
4. **Cost.** A large merge is millions of ops, and with free rotation the cost is the oriented
   bounding box's volume, not the stamp's voxel count. It needs the fill's budget discipline,
   and the fill's lesson about counting *region voxels, not probes*.
5. **Scale.** The gizmo has no scale handles, and scaling voxels is a resample. Generated
   shapes resize by regenerating (exact, free); lifted ones do not resize in v1.

## Build order

1. `FloatingStamp` + the shared placement block + the gizmo predicate + merge/cancel/keep,
   with one hardcoded box. That is the whole spine and it is testable alone.
2. Shape rasterisers: Box, Sphere, Cylinder, Cone, Wedge/Ramp, Pyramid, Torus. Shape panel.
3. Region: box selector + Copy/Cut → lift.
4. Region: Face and Volume selectors on the existing gathers.
5. `Subtract` merge mode.
6. Selection-scoped Fill and Delete.

## Self-test

A `STAMPTEST` block under the existing `EDITOR_SELFTEST` convention, covering what is
invisible from the outside — a merge off by one cell produces a plausible result in the wrong
place, which is the same failure class the paint-coordinate test already guards:

- lift a known box and merge it back at zero offset — voxel count and contents identical;
- merge at a `+N` voxel offset — exact expected count, no strays;
- four 90° rotations compose to the identity;
- cut, then cancel, restores the baseline;
- undo of a merge restores the baseline;
- a **hollow** shape at 37° has no holes in its surface — the pull-rasterisation test, and the
  one that fails loudly if anyone reimplements the merge as a forward map.

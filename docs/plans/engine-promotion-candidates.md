# Engine promotion candidates, from the SceneEditor

Status: **review in progress.** Items 1-8 have been ruled on; 9-20 have not been discussed yet.
Nothing here is implemented.

## What this is

A pass over `docs/examples/SceneEditor/` (main.cpp ~25k lines, plus `brush.{h,cpp}` and
`edit_history.{h,cpp}`) asking one question of each piece: **is this a mechanical, generic
component that ProjectV should carry, or is it this application's design?**

The test applied throughout is the one that framed the review: a Stone Crack brush is not engine,
a generic brush system might be — but only if it can be justified by *vastly different* use cases
served by the same machinery. Where a candidate could not clear that bar, it is recorded as
example-only rather than quietly promoted.

Sources read: `include/` in full (core, data_structures, graphics, utils), the editor's structures
and helpers, `docs/plans/brush_system.md`, `docs/plans/render-target-sizing.md`.

---

## Decisions so far

### 1. Component voxel lattice — **engine**

One continuous voxel space per component, unifying loose Chunk and Grid, with world↔voxel
conversion defined *even where no chunk exists yet*.

In the editor: `ComponentVoxelSpace`, `resolveComponentVoxelSpace`, `componentVoxelToWorld`,
`worldToComponentVoxel`, `worldDirectionToComponentAxis`, `componentVoxelToChunk`,
`queryComponentVoxel`, `chunkVoxelToComponentCoord`, `pickToComponentVoxelCoord`
(main.cpp ~7725-8015).

**Why it is engine.** The engine's own two APIs do not compose. `utils::pickVoxel` reports a hit as
`(ChunkHandle, chunk-local ivec3)`; `utils::queueVoxelAdd` takes *component-space* continuous
coords. Going from a hit to an edit requires exactly this glue, and today every caller has to
rewrite it — correctly handling `floorDiv`/`floorMod` bucketing, `originCellCoord` rebasing after a
negative grid expansion, and the Chunk-vs-Grid split.

The "defined before the cell exists" property was accepted as part of the promotion: additive
editing depends on it (the cell is created *because* something was written there).

**Watch for:** `ComponentVoxelSpace::grid` is a pointer into `scene.grids` and does not survive an
edit — `convertChunkToGrid` can reallocate the vector. Resolve, use, then mutate. An engine version
should either document this as loudly as the editor does or hand back a grid index instead.

### 2. Boolean op resolution (the fold) — **engine**

Evaluating a stack of `BooleanOp::{Union,Subtract,Intersect}` children into a voxel result.

In the editor: `FoldAccumulator`, `Fold`, `foldChildren`, `foldNode`, `pullPartIntoLattice`,
`pullLeafIntoLattice`, `rebuildResult` (main.cpp ~10902-11290).

**Why it is engine.** `BooleanOp` already lives in `ComponentRecord` (scene.h) *and* in
`ComposeComponent` (compose.h), and `compose_io` round-trips it to disk. So the engine writes a
format describing an assembly it cannot itself evaluate: any runtime that loads such a
compose.json renders the placed parts instead of the composed result — a degraded picture with no
stated cause.

Beyond the editor: runtime destruction (subtract a sphere from a wall), procedural assembly, and
flattening for LOD or export are the same fold.

**Notes carried over from the editor's implementation, worth keeping:**
- The first contributing row seeds the accumulator whatever its op says. An empty accumulator
  intersected or subtracted-from resolves to nothing, which is an outcome with no visible cause.
- Grids must fold, not be skipped. A Chunk becomes a Grid on its own the first time a sculpt
  reaches past its resolution (`convertChunkToGrid`), so excluding grids means an ordinary `.data`
  can silently stop composing.
- Both budgets (cells walked, cells in the result) need to be reportable *separately and by row* —
  a single "over budget" tells the caller neither which knob to turn nor which row was at fault.
- `FoldAccumulator` is an open-addressed table keyed on packed voxel coords with splitmix64 mixing
  and tombstones dropped on rehash. The mixing is load-bearing: raw packed coords put a whole row
  of the result into one probe chain.

Open question deferred: whether the engine ships the editor's *live* resolve (cached per part,
settling-gated, invalidated on transform/palette change) or only a bake-style
`flatten(subtree) -> component`. The live machinery is where the editor-specific policy lives.

### 3. Component visibility — **engine**

A supported way to stop drawing a component while keeping it fully readable.

In the editor: `setComponentRendered`, `applyIsolation` (main.cpp ~10186, ~4813).

**Why it is engine.** Both mechanisms are already engine state — `SceneGrid::rendered` and
membership in `Scene::looseChunks` — with no API over either. The distinction that matters is
already documented in `picking.h`: hiding must not be done by clearing `Chunk::alive` or blanking
`cellToChunk`, because the fold and picking both read through those. Culling, LOD, gameplay
hiding, and the editor's isolate all want the same switch.

Small, mechanical, no policy.

### 4. Content bounds — **engine**

The tight AABB of a component's or chunk's *actual voxels*, as distinct from its chunk box.

In the editor: `chunkContentBounds`, `componentContentBounds`, `componentAddressableBox`
(main.cpp ~11295-11405, ~12659).

**Why it is engine.** `utils::scene_query` already answers voxel count and world position; extent
is the missing sibling. Camera framing, physics broadphase, culling, choosing a bake resolution,
and centring a shape in its chunk all need it.

Decision was plain promotion (walk the tree on call), *not* caching it on the blob. If profiling
later argues for a cached bound maintained by the edit path, that is a separate change — the
tree-walk version is the correctness baseline it would have to match.

### 5. Reversible-edit snapshots — **deferred, and reframed**

Original candidate: promote `capturePaletteSnapshot`/`restorePaletteSnapshot`
(`edit_history.{h,cpp}`), on the grounds that a palette removal renumbers slots, rewrites material
bytes in every blob the component owns, and COW-forks geometry shared with another component —
none of which is recoverable without knowing blob refcounts and brickmap internals.

**Redirected during review, and this is the more interesting idea:** this may be the wrong shape
for the right mechanism. The direction to explore instead is **layers in a `.data`**.

> Per-voxel animation is wanted later — grass and leaves swaying, water rippling. A `.data` that
> can hold **multiple layers**, which can be **toggled**, **deleted**, or **merged**, looks like
> almost the perfect mechanism for that, and a snapshot/restore is arguably a degenerate case of
> it (capture = a layer; restore = re-select the layer; commit = merge down).

Needs more thought before anything is written. Questions to answer first:

- What is a layer, exactly: a whole parallel tree64 + materialIDs pair per layer, or a sparse
  overlay of differences against a base? The first is simple and costs memory per layer; the
  second is cheap and makes "what is solid here" a resolve rather than a read — which the shader
  would have to do per voxel.
- Does the *GPU* see layers, or only the CPU? Sway/ripple implies the shader must, which makes
  this a format and traversal change, not just an editing convenience.
- How do layers interact with the existing COW/refcount pool (`GeometryBlob`), with `renderLOD`
  truncation, and with the palette (one palette per component, or one per layer)?
- Does the undo problem actually collapse into this, or does undo still need palette-renumbering
  capture as a separate thing? A palette removal changes *slot numbering*, which is data every
  layer would reference — that may not be expressible as a layer at all.
- Relationship to `BooleanOp`: a stack of layers folded down is very close to item 2's fold. Worth
  checking whether these are one mechanism before building two.

Until that is settled, the editor keeps its own palette snapshot.

### 6. Cross-component geometry merge, with palette remapping — **engine**

Combining two components' voxels, which requires remapping one's material slots into the other's
palette.

In the editor: `planCellWrite`, `mergeContentsToData`, `bakeNodeInto`, `ensureBrushPalette`
(main.cpp ~12725-13140, ~17075).

**Why it is engine.** The engine owns the palette format, the per-component slot space, and the
255-entry ceiling (`MAX_MATERIALS_PER_COMPONENT`), so it is the only place slot remapping can be
written once and be right. The README already records cross-scene component copy as *disabled*,
pending engine-side blob/palette support — this is that support. Merge, import, bake, instancing
and cross-document copy are all the same operation underneath.

Decision was the full thing (geometry + palette), not the palette-remap primitive alone.

**Must report overflow by name, not by failing.** A merge that would exceed 255 slots has to say
which component and how many entries over, because the caller's options (dedupe, quantise, split)
depend on that.

### 7. Dense volume window — **engine**

Read a box of a component into a flat `solid[] + material[]` array; write the difference back.

In the editor: `SculptScratch`, `snapshotSculptScratch` (main.cpp ~8877-8935), and `BrushField`,
`snapshotBrushField` (~16874-17010).

**Why it is engine.** Any pass that reads a *neighbourhood* re-reads most of what its neighbour
just read: a 3×3×3 count re-reads 18 of 27 cells. Asked of the tree64 one cell at a time that is
~28 descents per cell — the editor measured a radius-10 pass at half a million descents per tick,
dropping to ~17k with the dense copy. Erosion, cellular automata, physics, procedural growth,
filtering and meshing all pay that same 28× without it.

Purely mechanical: the engine provides the window, the caller decides what happens in between.

Decision was read *and* write sides, not read-only. The write side should stay a difference
against the snapshot rather than a blind blit, so that the existing
`queueVoxelAdd`/`queueVoxelRemove` → `updateScene` path (one queue, one GPU flush) is preserved.

**Design note:** out-of-box reads as empty. The editor guarantees nothing is ever written within
one cell of the boundary, so no decision depends on that — an engine version should make the
margin an explicit parameter rather than an invariant callers must know about.

### 8. Volume morphology — **engine, as a generic kernel pass**

Not three named operators, but **one "apply this neighbourhood function over a region" pass**, with
majority-smooth, dilate and erode shipped as callers of it.

In the editor: `SculptOperator`, `runSculptPass` (main.cpp ~8854-9074), plus the strength mapping
helpers `sculptSmoothKernelRadius` / `sculptSmoothCutoff` / `sculptSmoothThreshold`.

**Why the kernel-pass shape.** It makes novel operators cost nothing, which is the difference
between a mechanism and a menu. The three named operators then become data — exactly the
Stone-Crack-vs-brush-system distinction applied one level down.

**The one rule the pass must enforce:** every cell is decided against the state at the *start* of
the pass; changes are gathered and written afterwards. Deciding against a half-updated array makes
the result depend on loop order — a pass sweeping +x smooths differently from one sweeping −x, and
a dilate runs away across the whole region in a single tick instead of growing one layer. This is
not an optimisation detail; it is what makes the operator well-defined.

Builds directly on item 7 (the pass runs inside the dense window).

---

## Not yet discussed

Listed in the order they were going to be raised. Justification and the strongest counter-argument
are recorded so the discussion can resume cold.

### 9. Derived per-voxel fields — skin depth, crevice, gradient normal

`BrushField`, `brushCreviceAt`, `brushNormalAt` (main.cpp ~16874-17055).

Skin depth (0 on the surface, 1 one voxel in), crevice (local solid fraction, 0 exposed .. 1
buried), and a surface normal from the local solid gradient. **For:** all three are questions about
the *volume*, not about brushes — procedural texturing, runtime shading hints, "is this voxel
exposed or buried" gameplay checks, and erosion sims want the same numbers. **Against:** the
margins they need (`maxSkinDepth`, `creviceRadius`) are cost knobs that only make sense alongside
a windowed read, so this may just be a documented use of item 7 rather than its own API.

### 10. Voxel set, and predicate-driven flood fill

`VoxelSelection` (main.cpp ~927), `VisitedVoxels` (~8016), `gatherFaceRegion` (~8077),
`collectPaintTargets` (~8145), `gatherVolumeRegion` (~13309).

A bitset over a bounding box, plus connected-component gathering driven by a caller-supplied
predicate. **For:** the set is a plain data structure, and flood fill by predicate covers
destruction (find the connected piece that broke off), room/volume detection, AI navigation, and
editing with one mechanism. `VisitedVoxels` is specifically a bit-per-voxel visited set allocated
only for chunks the fill enters — the coordinate-hashing version it replaced put a hard ceiling on
fill size. **Against:** "flood fill scoped by material vs. by solidity" is editor policy; only the
predicate-parameterised form is mechanical.

### 11. Lattice symmetry group

`LatticeMap`, `composeLatticeMaps`, `buildSymmetryGroup`, `SymmetryFrame` (main.cpp ~1011-1170).

Signed axis permutations with integer offsets — the only symmetries a voxel grid can have without
resampling. Exactly 48 of them, closed under composition, every one exact. **For:** pure
mathematics with no editor in it; procedural generation of symmetric structures, tiling rulesets,
instancing and mirror modelling all want it. The doubled-origin trick (storing 2× the plane
position so parity distinguishes through-cell-centres from along-cell-boundaries) is the kind of
detail that gets re-derived wrongly every time. **Against:** `SymmetryFrame` with its per-axis
toggles is UI state and should not go; only `LatticeMap` + the group builder.

### 12. Implicit-shape voxelization

`insidePrimitive`, `rasterisePrimitive`, `centrePrimitiveInChunk`, `smallestChunkResolutionFor`,
`centreBakeInChunk` (main.cpp ~9840-9996).

**For:** "fill this region of voxels where a predicate holds" is the mechanism, and explosions,
terrain brushes, spawn volumes and tools all use it. The auto-fit helpers (smallest power-of-four
resolution for an extent; centring content in its chunk so a gizmo pivot coincides with the shape)
are needed by anything that creates geometry programmatically. **Against:** the seven shapes
(box/sphere/cylinder/cone/wedge/pyramid/torus, hollow with wall thickness) are a *menu* — content,
not mechanism. Promote the predicate rasteriser; the shapes are data, and arguably example-side.

### 13. Render targets sized to a surface, not the window; several renderers at once

`EditorRenderers`, the deferred viewport resize, `getViewportTexture` (main.cpp ~2636-2710,
~16015). Already flagged in the README ("its render targets follow a panel, not the window. Worth
fixing engine-side eventually") and largely addressed by `docs/plans/render-target-sizing.md`,
which landed the `sizeMode` schema and per-pass view rects. **Remaining:** the editor keeps two
`ConstructedRenderer`s alive (viewport + path tracer) and switches per tab; whether that is an
engine-supported arrangement or an accident that happens to work needs deciding.

### 14. GPU texture readback

`renderSaveTexture` and the frame-latency handling in `EditorState` (~1440-1470) and `render()`.

**For:** screenshots, thumbnails, baked results, and any GPU→CPU path need it, and the naive
version *segfaults*: a texture created with `BGFX_TEXTURE_RT | BGFX_TEXTURE_READ_BACK` comes back
as an invalid handle, and the framebuffer that then attaches it dereferences the hole — during
renderer construction, with nothing reported. The correct shape (a separate staging texture, blit,
and holding the buffer across the frames until `bgfx::readTexture`'s returned frame lands) is
exactly the kind of thing that should be written once. **Against:** none identified.

### 15. Progressive accumulation reset

`frameCameraLastMovedOn`, `renderSettingsChanged`, `renderParamsChanged`, `cameraMovedByInterface`.

**For:** any progressive/accumulating renderer needs "the frames already averaged were rendered
under the old state, so start the mean again" — and getting it wrong shows up as a setting fading
in over 64 frames instead of appearing. **Against:** it is three booleans and a frame counter;
possibly too thin to be worth an API, and the *policy* about what invalidates is app-specific.

### 16. CPU/GPU camera parity

`viewportRayThroughUV`, `cameraOrthoHeight`, `cameraOrthoBackoff` (main.cpp ~2288-2360),
`worldToViewportPixel` (~3647).

`utils::rayDirectionThroughImage` exists and explicitly promises to match `pjv_utils_DDA.sc`'s ray
generation — but only for perspective. The editor added orthographic and isometric, so picking and
the shader can now disagree. And there is no engine `worldToScreen`, which is the *inverse* of a
function the engine already ships and is needed by anything drawing an overlay over a rendered
image. **For:** the parity guarantee is the whole point of `rayDirectionThroughImage` existing;
a partial guarantee is worse than none. **Against:** none identified.

### 17. Camera controller

`updateCamera`, `updateCameraPan`, `updateCameraZoom`, `frameScene`, `measureSceneBounds`,
`aimCameraAtFocus`, `setCameraProjection`, `CameraFraming` (main.cpp ~2189-2910).

`data_structures/camera.h` is a five-field struct plus a global `inline Camera cam`. The editor has
fly/orbit/pan/zoom, three projections, frame-to-scene-bounds, and a speed that scales with scene
size. **For:** every example reimplements some of this. **Against:** input handling and feel are
app design, and the engine would be taking on GLFW. The defensible core is probably
`frameScene`/`measureSceneBounds` (which is item 4 plus arithmetic) and the projection math, not
the input loop.

### 18. Programmable brush host

`brush.{h,cpp}`, `docs/plans/brush_system.md`, `external/lua`.

**This is the item the Stone-Crack test was posed about, and it should be split before being
answered.** Three separable pieces:

- **The Lua host** — sandboxing, hot reload, the instruction-count watchdog with its per-call time
  deadline (a pure instruction budget is *wrong*: Lua's count hook runs across the whole state, so
  a stroke of 100k honest calls trips a 2M budget as reliably as one infinite loop, killing the
  stroke at a random voxel). Strongest engine argument: data-driven modding, runtime procedural
  content, and tooling all want a scripted extension point, and this one is already correct about
  the hard part.
- **The evaluation contract** — `BrushContext` in, `BrushVerdict` out, with the read fields
  *declared up front* so the expensive ones can be computed once over a dense box. This is a
  generic "volumetric field evaluator" and would serve C++ callers with no Lua present. Probably
  the strongest promotion candidate of the three, and it composes with items 7 and 9.
- **The editor-facing half** — `BrushParam` widget metadata, `BrushLibrary` folder watching,
  sidecar `.params.json`, material-role-to-palette binding. Almost certainly example-only.

The design's own load-bearing constraint is engine-shaped, though: a brush returns a material
*index*, never a colour, because free-form RGB would spend a 255-entry palette in a few thousand
voxels and then hard-fail with nothing painted. That is a fact about `MAX_MATERIALS_PER_COMPONENT`,
so whoever owns the palette has an interest in the contract.

### 19. Generic undo/redo command log

`EditHistory` (`edit_history.h`) — closure pairs, memory budget with oldest-first eviction,
coalescing by key within a time window.

**For:** every tool and any transactional system wants it. **Against:** nothing about it is
voxel-specific, so a voxel engine carrying a general command log is scope creep; the part that
*is* engine-specific is the snapshot capture, which is item 5 and now folded into the layers
question.

### 20. Ray/line/plane intersection into `core::math`

`closestPointOnAxis` — **defined twice in main.cpp**, at ~9177 (extrude) and ~14091 (gizmo) — plus
`intersectRayPlane`, `distancePointToSegment`, `nearestAxisAlignedRotation`.

Small, obviously generic, and the duplication is the code asking for it. `nearestAxisAlignedRotation`
(snap a rotation matrix to the nearest of the 24 axis-aligned ones) pairs naturally with item 11.

---

## Suggested build order

Items 1, 3 and 4 are small, self-contained, and everything else leans on them — 1 in particular is
a prerequisite for 2, 6, 7 and 9. Then 7, then 8 on top of it. Then 2 and 6, which are the two
large ones. 5 stays open until the layers question is answered; the rest are undiscussed.

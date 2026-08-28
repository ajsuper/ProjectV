# Agent Notes

## Build Commands

Everything builds from the top-level CMake project. There are no per-example Makefiles and no
per-example shader scripts any more -- one target, one shader rule.

```bash
cmake --preset dev            # configure (submodule deps, examples on)
cmake --build --preset dev    # build the library, the examples, and their shaders
```

The first build is slow: it compiles bgfx, bx, bimg and shaderc from `external/bgfx.cmake`.

Build just the library:
```bash
cmake --build build --target projectV
```

Examples land in `build/examples/<name>/`, with their renderer folders and scenes staged
beside the binary. Run them from there -- the engine resolves relative paths against the
working directory, and renderer folders name their shaders relative to it in `resources.json`:

```bash
cd build/examples/scene_previewer && ./scene_previewer scenes/StonehillCastle
cd build/examples/path_tracer     && ./path_tracer
cd build/examples/scene_editor    && ./scene_editor
```

Presets: `dev` (default), `release` (`PROJV_LOG_MINIMAL=ON`), `vcpkg` (dependencies through
`find_package`; the only configuration that can be installed -- see the note in `CMakeLists.txt`).

Options: `PROJV_BUILD_EXAMPLES`, `PROJV_USE_X11`, `PROJV_LOG_MINIMAL`, and the seven
`PROJV_LOG_<CATEGORY>` switches. They are `PUBLIC` on the target, so consumers inherit them.

MeshVoxelizer and SceneEditor need their submodules; without them the configure step skips
each with a message naming what to check out.

```bash
git submodule update --init --recursive
```

Manual tests (need a display):
```bash
cd tests/manual && make && ./exit_path a && ./exit_path b
```

## Codebase Conventions

- Namespace `projv` for core types, `projv::utils` for utilities, `projv::graphics` for GPU.
- Chunk handle = index into `Scene.chunks` (stable).
- Component handle = index into `Scene.components` (stable).
- Geometry pool blobs are refcounted; `chunk.geometryPoolIndex < 0` = unpooled.
- `chunkQueue` on Chunk is the legacy edit staging area (to be removed after P1 verification).
- `editQueue` on ComponentRecord is the new per-component edit queue (P1+).
- `forkBlob` creates a COW copy without decrementing the original's refCount.

## Phase 1–5 Status (2026-07-11)

### Delivered (Phase 1)
- `scene.h`: Added `PendingVoxelOp`, `ComponentEditQueue`, `DataReference`, `ComponentRecord::editQueue`/`dataRefID`, `Scene::dataReferences`, `forkBlob()`.
- `voxel_math.h/cpp`: Added `floorDiv`/`floorMod` for correct negative-coordinate cell bucketing.
- `include/utils/editing.h` + `src/utils/editing.cpp`: `queueVoxelAdd`, `queueVoxelRemove`, `updateScene` (loose chunks only, always-COW, CPU-only).
- `CMakeLists.txt`: `projectV-editing` static library target.
- `docs/examples/editing_p1/{main.cpp,Makefile}`: Test driver.
- `AGENTS.md`: This file.

### Delivered (Phase 2)
- `expandGridToInclude(SceneGrid&, core::ivec3, Scene&, int)` — expands a grid in both directions, shifts origin so existing chunks don't move, maintains `originCellCoord` for linearization.
- `SceneGrid::originCellCoord` — tracks the block-space coordinate at grid.origin after (possibly negative) expansion.
- `queueVoxelAdd`/`queueVoxelRemove` now accept both Chunk and Grid components.
- `applyComponentQueue` extended for Grid components: cell bucketing via `floorDiv`/`floorMod`, grid expansion, per-cell COW fork + rebuild, automatic new-cell chunk creation.
- `ensureDataReference` extended for Grid components (reads resolution from first populated cell).
- Test driver updated with Sponza grid acceptance test + full programmatic grid test (expansion, COW fork, refCount, dataRefID, origin shift, voxel counts).

### Delivered (Phase 3)
- `convertChunkToGrid(Scene&, ComponentHandle)` — converts a loose Chunk-kind component to a 1-cell SceneGrid. Sets up grid fields (origin, cellSize, rotation, dims=1), registers in `scene.grids`, updates chunk's `gridIndex`/`cellIndex`, removes from `scene.looseChunks`, and flips `comp.kind` to Grid.
- `applyComponentQueue` Chunk path: now checks all ops for overflow (any axis >= resolution or < 0). If overflow detected, calls `convertChunkToGrid` and falls through to the Grid path (same editQueue, same call).
- Test driver: 20-check programmatic loose-chunk overflow conversion test (P3 scope) — verifies conversion to Grid, grid expansion to (2,1,1), COW fork of original chunk, new cell creation for overflow, refCount invariants, loose count decremented, origin unchanged.

### Delivered (Phase 4)
- `GPUChunkHeader`: replaced `padding[2]` with `uint32_t dataRefID; uint32_t padding[1]` — same total size, same layout.
- `makeHeader`: updated signature to `(const Chunk&, const GPUBlobRange&, const Scene&)`; populates `dataRefID` from the chunk's `componentHandle` → `ComponentRecord::dataRefID`.
- `buildDataAndHeaderTextures`: passes `scene` to the updated `makeHeader` call.
- `rebuildSceneTextures(Scene&, GPUData&)` — calls `buildDataAndHeaderTextures` + `syncSceneTables` to rebuild GPU texture content from CPU state after edits, reusing existing samplers. Declared in `gpu_interface.h`.
- Shader `GPUChunkHeader` in `pjv_utils_DDA.sc`: updated to match CPU struct (`uint dataRefID; uint padding[1]`).

### Delivered (Phase 5)
- `GeometryBlob::dirty` (in `scene.h`) — per-blob flag set when a blob is new or changed (set in `forkBlob()` and `internChunkGeometry()`).
- `GPUData::uploadedChunkCount` (in `gpuData.h`) — watermark for new-chunk header detection.
- `flushSceneUpdates(Scene&, GPUData&)` (in `gpu_interface.h/cpp`) — incremental GPU upload:
  - `uploadDirtyBlobs` — iterates dirty blobs, allocates ranges from persistent `RangeAllocator`, uploads only changed texels via `bgfx::updateTexture2D` with row-by-row wrapping.
  - `updateDirtyHeaders` — rewrites only header rows for chunks whose pool blob was just uploaded or that are new.
  - `growDataTextures` / `growHeaderTexture` — full repack fallback when allocators are full (rare, amortized O(1) via `withHeadroom`).
- `rebuildSceneTextures` kept as documented fallback.
- `docs/examples/edit_demo/main.cpp` — switched to `flushSceneUpdates`.
- `docs/examples/editing_p1/main.cpp` — 8 new P5 dirty-flag assertions (all pass).

### Delivered (Phase 6)
- `scene.h`: Added `ComponentKind::Asset`. Added `name`, `parent`, `children`, `localPosition`, `localRotation`, `localScale` to `ComponentRecord`.
- `compose.h`: Added `name` to `ComposeComponent`.
- `compose_io.cpp`: Parse `"name"` from compose.json. Auto-generate names from filenames with sibling disambiguation. Rewrote `loadComposeFromDisk` to create `ComponentRecord`s for every compose.json entry (including Asset folders). Parent/child linking. Direct chunk creation in `scene.chunks` (tree order). Rebuild `looseChunks` list from tree after assembly.
- `include/utils/scene_query.h` + `src/utils/scene_query.cpp`: New module — `getComponentPath`, `findComponentByPath`, `findComponentsByName`, `listComponents`, `getComponentVoxelCount`, `getComponentWorldMatrix`/`Position`/`Rotation`, `setComponentPosition`, `setComponentRotation`, `setComponentTransform` (with subtree rebake).
- `src/utils/editing.cpp`: Skip `ComponentKind::Asset` in `applyComponentQueue`.
- `CMakeLists.txt`: Added `projectV-scene_query` library target.
- `docs/examples/editing_p1/main.cpp`: 21 P6 assertions (names, paths, transforms, world matrix, voxel counts, listComponents, transform mutation + restore). Skip Asset kinds in `findComponents`.
- `docs/examples/edit_demo/main.cpp`: Skip Asset kinds in component search.
- `docs/examples/editing_p1/Makefile`: Link `projectV-scene_query`.
- `AGENTS.md`: This entry.

### Delivered (Logging System)
- `include/core/log.h`: Replaced spdlog re-export with category-based template
  functions (`trace`, `perf`, `edit`, `render`, `info`, `warn`, `error`). Each
  gated by `#if defined(PROJV_ENABLE_*)` — empty body when disabled, eliminated
  by compiler. Tags (`[TRC]`, `[PRF]`, `[EDT]`, `[RND]`, `[INF]`, `[WRN]`,
  `[ERR]`) baked into functions.
- `CMakeLists.txt`: Seven `option()` calls (all ON by default) +
  `PROJV_LOG_MINIMAL` preset (disables everything except WARN+ERROR) +
  `add_compile_definitions` to emit defines project-wide.
- Migrated all existing call sites: manual `[PERF]`/`[EDIT]` tags removed,
  `core::debug` → `core::trace`, `core::critical` → `core::error`, direct
  `spdlog::` calls → `core::` wrapper, `spdlog::set_level()` calls removed.
- `perform_renderer.cpp`: Per-frame render logging gated with `#if` + on-change
  detection (static bool renders once).
- `docs/examples/edit_demo/main.cpp` & `docs/examples/PathTracer/main.cpp`:
  Frame-time perf summary every 100 frames with `#if defined(PROJV_ENABLE_PERF)` guard.
- `CODING_STYLE.md`: Updated logging section with category table and gating
  guidance.
- `AGENTS.md`: This entry.

### Delivered (Scene Editor — shell, 2026-08-01)
- `external/imgui`: Dear ImGui 1.92.9b **docking branch** as an engine submodule (intended for full
  engine integration later; no engine library links it yet).
- `docs/examples/SceneEditor/`: dockable editor shell — Viewport / Scene Hierarchy / Inspector /
  Statistics panels, File ▸ Load Scene… browser, runtime scene swap, fly camera gated to the
  Viewport panel.
- `imgui_impl_bgfx.{h,cpp}`: ImGui renderer backend for bgfx (bgfx's own lives in its example
  framework). Implements the 1.92 `ImTextureData` protocol; draws into bgfx view 200.
- `editorRenderer/`: the ScenePreviewer's renderer with the display pass retargeted from the back
  buffer (`-1`) to FBO 3, so ImGui can draw the scene as an image inside a dock node.
- Note: `resizeFramebuffersAndTheirTexturesIfNeeded` couples texture resize to `bgfx::reset` and
  leaks the framebuffer handles it replaces; the editor has its own `resizeViewportTargets` because
  its render targets follow a panel, not the window. Worth fixing engine-side eventually.

### Delivered (Scene Editor — palette editing + undo, 2026-08-01)
- `utils/material.h/cpp`: `addMaterial`, `setMaterialColor`, `setMaterialName`, `countMaterialUsage`,
  `findMaterialChunks`, `removeMaterial`. Usage counts walk the tree (a uniform leaf stores one
  material byte for up to 64 voxels, so counting `materialIDs` counts bytes, not voxels).
  `removeMaterial` renumbers slots across every blob of the component and copy-on-writes any blob
  shared with another component.
- `graphics::updatePaletteEntry` (gpu_interface): single-texel palette write for size-preserving
  recolours — flushSceneUpdates rebuilds the whole palette texture and dirties every chunk header,
  which is too much for a per-frame colour drag.
- `utils/picking.h/cpp` (new lib `projectV-picking`): `queryVoxelMaterial` (CPU tree64 point query,
  mirrors fetchVoxelColor), `pickVoxel` (chunk AABB sort + Amanatides-Woo DDA), and
  `rayDirectionThroughImage` (must stay identical to rayStartDirection in pjv_utils_DDA.sc).
  Verified against `countMaterialUsage` on two scenes: 0 mismatches over 1.7M voxels.
- Editor: palette grid, per-slot voxel/chunk breakdown, eyedropper, undo log
  (`edit_history.{h,cpp}` — closure-based records, coalescing, snapshot undo for destructive edits).
- **Scene loading is two-phase** (release → wait 8 frames → build). bgfx frees a destroyed texture
  only after the frames referencing it complete, so building the new scene in the same frame keeps
  both resident; a 3.2 GB scene reloaded that way crashed the Vulkan driver on an 8 GB card.
- Caution: `lib/` was stale (several archives predated header changes). Rebuild everything
  (`cmake --build build`) after touching engine headers — a partial rebuild gave an ABI mismatch.

### Delivered (Scene Editor — selection outline, 2026-08-01)
- Editor: selecting a Scene Hierarchy node outlines every `.data` box it covers (itself, every cell
  of a Grid, or every leaf beneath an Asset folder) in yellow in the Viewport — `collectLeafChunks` +
  `worldToViewportPixel` (the exact inverse of `rayStartDirection` in pjv_utils_DDA.sc) + a 12-edge
  cube draw in `main.cpp`. Inspector gained World rotation and a "Boxes" count (same set).

### Delivered (Scene Editor — editable transform, real bugfix, 2026-08-01)
- **Real bug, not a rendering artifact**: `drawHierarchyNode`'s click handler called `IsItemClicked()`/
  `IsItemToggledOpen()` *after* drawing the `"(chunk)"` kind suffix via `TextDisabled`, so both
  checked that tiny label's rect (ImGui's "last item"), not the tree node row. Clicking a node's own
  name — the natural place to click — never registered. This is what actually caused "no outline, no
  Inspector content" from the previous round, not a font issue. Fixed by capturing both flags
  immediately after `TreeNodeEx`. Checked every other `IsItemClicked`/`IsItemHovered` call in the
  file; this was the only one with something drawn in between.
- Font glyphs: root cause still not nailed down (recheck if it recurs), but ImGui's on-demand glyph
  bake was intermittently leaving specific glyphs (capital I, W) with correct metrics + correctly
  rasterized CPU pixels yet no visible ink on screen. Rather than ship a guess, the base ASCII +
  Latin-1 range is now force-baked via `ImFontBaked::FindGlyph` before the first frame, so that path
  is never taken for anything we actually use.
- `utils::setComponentScale` + extended `setComponentTransform` (now 4 args: pos, rot, scale) --
  editable Scale needed a **real engine fix**, not just UI: `rebakeSubtree` never applied
  `localScale` to `Chunk::header.scale` / `SceneGrid::cellSize`, and separately would have produced a
  silently-wrong quaternion the moment scale != 1.0 (quat_cast fed a matrix with scale still baked
  into its columns). Fixed via proper decomposition (extract scale by column length, normalize before
  quat_cast — same technique `loadComposeFromDisk` already uses). New runtime-only fields
  `Chunk::nativeScale` / `SceneGrid::nativeCellSize` (NOT `ChunkHeader`, which is disk-persisted) hold
  the transform-independent size so repeated edits don't compound.
- Verified via `editing_p1`: stashed my changes, confirmed 5 pre-existing unrelated failures on bare
  `main`, restored, confirmed identical 5 after — zero regressions. Added 3 permanent P6 checks for
  `setComponentScale` (2x, 0.5x, restore), all pass.
- Editor: Inspector's local Position/Rotation/Scale are now live-editable (drag, undoable,
  coalescing). Rotation shown/edited as Euler degrees, cached per-selection (not recomputed every
  frame — would fight the drag and jump at gimbal lock). "Reset transform" button.
- Editor: selecting a Scene Hierarchy node outlines every `.data` box it covers (`collectLeafChunks`
  + `worldToViewportPixel`, the exact inverse of `rayStartDirection`) in yellow in the Viewport.
  Inspector gained a "Boxes" count and read-only World transform display.

### Delivered (Scene Editor — tools, layout, viewport selection, 2026-08-02)
- **Tool modes** (`EditorTool`: Select/Move/Sculpt/Paint) decide what a left-click in the Viewport
  means, replacing four features negotiating for the same button. `Ctrl+Q/W/E/R` — Ctrl-qualified
  because the bare letters fly the camera (`W/A/S/D/R/F` while the right button is held), so an
  unqualified chord would change tool mid-fly-through. **`Ctrl+R` reload moved to `Ctrl+Shift+R`.**
- Tool strip: four clickable icons floating down the left edge of the scene image, drawn from
  ImDrawList primitives (same reason as the render toggles — ImGui's default font is ASCII). The
  chrome is shared with the settings bar via `drawViewportIconButton`; the Tool panel repeats the
  same four buttons, which is why the icons live above the panels rather than beside the bars.
- **Viewport click-to-select**: `PickPurpose` (SelectComponent/SampleMaterial/PaintVoxel) replaces the
  single `pickRequested` bool; one block in the Viewport decides the purpose from armed-eyedropper →
  tool → Alt. Selecting from the viewport opens the selection's ancestors and scrolls the Scene
  Hierarchy to it (`revealSelectionInHierarchy`). A miss deselects only under Select.
- **Paint tool**: `queueVoxelAdd` → `updateScene` → `flushSceneUpdates`, undoable, coalescing per
  voxel. `pickToComponentVoxelCoord` converts a pick's *chunk-local* coord to the *component-space*
  coord the edit queue wants — for a Grid that means inverting `applyComponentQueue`'s
  `floorDiv(position, res)` bucketing against `grid.originCellCoord` and the chunk's `cellIndex`.
  Getting this wrong paints in a different cell than the one clicked; loose Chunks are the identity.
- **Layout**: right column split into three stacked nodes (Inspector / Tool / Palette) instead of
  three tabs. That is what let `drawSculptPanel`'s duplicated palette grid be deleted — it existed
  only because the panel could never be on screen at the same time as the Palette.
  `Sculpt` panel → `Tool` panel, window name `"Tool###ToolPanel"` so the visible title can follow the
  tool while the ImGui identity (and therefore the dock layout) stays put.
- Left column split: Scene Hierarchy above a new **Library** panel — a disk browser that lists a
  scene's components via `parseComposeJson` with **no geometry read and nothing uploaded**. Import is
  present but disabled with a tooltip: cross-scene component copy needs an engine-side blob/palette/
  handle merge that does not exist. This is also why multi-scene is a browser and not co-resident
  scenes — see the two-phase-load note above.
- Statistics dock panel → one-line **status bar** (`BeginViewportSideBar`) + optional floating window
  under View. `statusMessage` also floats over the viewport for ~3s (`drawViewportToast`), since most
  messages answer something the user just did there. History now owns the bottom node alone.
- **Breadcrumb** over the viewport: `scene > ancestor > selection`, every element clickable, with the
  active tool right-aligned.
- Gizmo gate changed from `inspectorTabVisible` to `activeTool == Move`; `inspectorTabVisible` is gone
  (it was a proxy for "is the Inspector the selected tab", meaningless now the three are all visible).
- Dockspace ID bumped to `EditorDockSpaceV2` so an existing `imgui.ini` rebuilds the default layout
  once rather than half-restoring it (old panels placed, new ones floating).
- Gotchas hit: `ImGui::SetCursorPos` is **window**-relative, not content-relative — an absolute y of a
  few pixels puts the breadcrumb behind the dock tab bar, drawn and invisible. And ProggyClean is
  ASCII-only, so em-dashes in *displayed* strings render as `?`; the status bar made the pre-existing
  ones obvious, and all displayed strings are now ASCII (comments are untouched).
- `Makefile`: links `projectV-editing`.

### Delivered (Scene Editor — paint modes, 2026-08-02)
- `PaintShape` (Voxel/Sphere/Cube/Fill) with per-shape settings in the Paint tool panel. Sizes are in
  **voxels**, not world units — a world-unit radius would cover a different number of voxels per
  component depending on `voxelScale`. World size is shown alongside.
- All four **only recolour voxels that already exist**; none creates one (that is Sculpt's job), and
  all stay inside the clicked component, because an edit queue belongs to a component.
- Fill is a 6-connected flood comparing **palette slots, not colours** — two entries holding the same
  colour are separate materials and stay separate regions. Face neighbours only; diagonal
  connectivity leaks a fill through the gap where two walls touch at an edge.
- Caps: radius 32, cube side 64, fill 4e6 voxels. The cost driver is the *scanned box*, not the
  painted count — every candidate coordinate is a tree64 descent to learn whether a voxel is there.
- **Fill bug found by the user and fixed (striped, incomplete fills).** Three compounding faults, all
  in the gather; the engine's apply path was verified exact throughout (0 wrong of 1.55M):
  1. The budget was measured against the *visited* set, which holds every rejected neighbour too --
     ~7x the region size. A nominal 1e6 limit stopped a fill at ~160k voxels of real region.
  2. The traversal was depth-first (a stack), so truncating it left long tendrils rather than a
     compact partial region. **That is what the stripes were: the traversal order.** Invisible from
     the outside, because a fill's shape is meant to look arbitrary.
  3. Visited was an `unordered_set` of packed coordinates -- ~40 bytes and a hash per voxel, which is
     what forced the budget low enough for (1) and (2) to bite.
  Now: BFS over `VisitedVoxels` (one bit per voxel, per chunk, allocated on entry), budget counting
  region voxels. StonehillCastle's ground layer went from 318,821 gathered to 1,556,106 with 0
  adjacency leaks. **None of this reproduces on a small region** -- the first round of tests passed
  on a 31-voxel sphere.
- `runFillSelfTest` (same `EDITOR_SELFTEST=1`, read-only): asserts a fill is adjacency-closed -- no
  solid same-slot voxel may touch the gathered set without being in it. Seeds on the component's most
  common material, because the small regions were always correct. 1,147,019 leaks before, 0 after.
- `ComponentVoxelSpace` + `resolveComponentVoxelSpace` resolve a component's voxel addressing **once
  per stroke**; `componentVoxelToChunk` is the inverse of `pickToComponentVoxelCoord`. A brush asks
  about coordinates nobody picked, so both directions are needed and they must be exact inverses.
- **Bug this caught (via the new self-test, before any user could hit it):** the reverse map first
  read a Grid's resolution from `scene.dataReferences[record.dataRefID]`. `dataRefID` is `-1` until a
  component's *first edit* assigns one (`ensureDataReference`), so on every freshly loaded scene the
  Grid path bailed and the brush found an empty world — 9962/9962 probes mismatched. It now reads the
  resolution off the first populated cell, the same fallback `ensureDataReference` uses.
- `runPaintCoordSelfTest` — opt-in (`EDITOR_SELFTEST=1`), read-only round-trip of both mappings.
  Passes 0/0 on Sibenik and LostEmpire (grids) and StonehillCastle (loose chunk). **Run it after
  touching either mapping or how a Grid derives its resolution** — nothing else in the editor would
  notice a broken inverse; it just paints a plausible region in the wrong cell.
- A stroke is queued in **one** `queueVoxelAdd`/`updateScene` pair: updateScene forks and rebuilds
  every chunk a queue touches, so per-voxel calls would pay that rebuild per voxel.
- Undo is a per-voxel previous-colour list (a sphere can span several materials), with the coordinate
  vector shared between the undo and redo closures via `shared_ptr` and `record.memoryCost` set — a
  million-voxel fill would otherwise be three copies and an unaccounted 36 MB in the history.
- Fixed a **pre-existing layout bug** copied from the sculpt panel: `SetNextItemWidth(-1.0f)` fills to
  the right edge, so a trailing `SameLine` + `TextDisabled` label is laid out past it and clipped —
  every such field in the Inspector and Tool panels was silently unlabelled. `fieldWidthBeside(label)`
  reserves the label's width; the cube rows all reserve the widest label's so the fields line up.
- Removed the `// TEMPORARY diagnostic` block in `render()` (frame-100 simulated 60-step drag on
  `scene.grids[0]`), which mutated the loaded scene on every run of a grid scene.

### Delivered (Scene Editor — sculpt strokes, 2026-08-02)
- **Sculpt tool is wired up**: left-click + drag in the Viewport adds or removes voxels. `Sphere` and
  `Cube` are brushes stamped along the drag; `Extrude` is a different interaction entirely (below).
- **The whole difficulty is that a stroke edits the scene it is casting into.** Every frame casts a
  ray, and by frame two the scene already contains the previous frame's dab: an additive drag hits its
  own deposit (nearer the camera than the surface), places on top of that, and walks a column of
  voxels back up the ray. Removal is the mirror — the ray falls through the hole it just made and eats
  a tunnel. Both are fixed by showing the ray the surface the stroke *started* against:
  - `utils::VoxelSolidityOverride` — new optional last parameter to `utils::pickVoxel`, a
    `bool(ChunkHandle, ivec3, bool solidInScene)` called for **every cell the DDA steps into**, not
    only solid ones (forcing an empty cell back to solid is half of what it is for).
  - `EditorState::sculptStrokeTouched` is the set it reads: every cell this stroke changed. Added →
    report empty; removed → report solid. Only the stroke's own component is overridden.
  - A voxel forced solid has **no material** (it is not in the tree): such a pick carries slot 0, so
    never hand a solidity override to anything that reads `materialSlot` (samplers, eyedropper).
- A stroke locks its **component**, **mode** and **colour** when the button goes down. An edit queue
  belongs to a component, so a drag that wandered onto a second object would otherwise split into two
  undo entries and write geometry where the user was not pointing.
- One `queueVoxelAdd`/`queueVoxelRemove` + `updateScene` **per frame**, carrying every dab that frame
  produced — `updateScene` forks and rebuilds every chunk a queue touches. One history entry **per
  stroke** (`shared_ptr` coords, `memoryCost` set), not per dab.
- Dabs are interpolated between frames at half the brush radius (`SCULPT_MAX_INTERPOLATED_STEPS = 48`),
  or a fast flick lays a dotted line of disconnected blobs. Past the cap the stroke thins rather than
  the frame stalling.
- `ComponentVoxelSpace` gained the **world lattice** (`latticeOrigin`/`rotation`/`voxelSize`/
  `coordOrigin`) + `componentVoxelToWorld` / `worldToComponentVoxel`. A component's cells and voxels
  share one lattice (`origin + R * (cellIJK * cellSize)`, see `applyComponentQueue`), so a coordinate
  is defined **whether or not its chunk exists yet** — which is what lets a stroke place the first
  voxel into an empty component and carry a hit across from a different object. `coordOrigin` is
  `originCellCoord * resolution`, non-zero only after a Grid has expanded downward.
- `pickToComponentVoxelCoord` split out `chunkVoxelToComponentCoord(scene, component, chunk, local)`,
  which also **rejects chunks that are not the component's** — the ray override is called with
  whatever chunk the traversal walked into, routinely somebody else's.
- Empty-space behaviour: a miss places the dab `sculptPlaceDistance` world units down the ray into the
  *selected* component. Without it a brand-new empty component could never be sculpted at all.
- `runSculptLatticeSelfTest` (`EDITOR_SELFTEST=1`, read-only): the lattice must put a voxel where its
  own chunk puts it (the expression `pickVoxel` inverts for `worldPosition`), and world↔voxel must
  round-trip. **A wrong `coordOrigin` on a downward-expanded Grid fails here and nowhere else** —
  every chunk stays individually correct and the whole component is simply offset from where the tool
  aims. 0/0 on Sibenik, LostEmpire (grids) and StonehillCastle (loose chunk).
- `runSculptStrokeSelfTest` (**`EDITOR_SCULPTTEST=1`, separate switch — this one edits the scene**, in
  memory, and undoes itself through the history). Runs a 20-frame stroke with the ray held still, four
  ways: add/remove × override kept/removed. The control clears `sculptStrokeTouched` between frames,
  which is precisely the tool with the fix taken out.
  - **Assert on where the ray still thinks the surface is, not on how far the placed voxels reach.**
    The two failures do not look alike from the geometry's side: climbing piles voxels toward the
    camera and shows in their extent, but tunnelling only does if there is something deeper to eat —
    and a voxelised surface is usually a *shell*, so the ray exits the scene instead. The first version
    of this test measured extent and reported the remove control as passing when it had bored straight
    through. Both failures share exactly one signature: the ray stopped agreeing with where the stroke
    started.
  - StonehillCastle: add control climbs 64.7 units toward the camera; remove control tunnels out of
    the scene entirely (`ray now nothing`). Sibenik (interior geometry) eats 31 units deeper instead.
    Overrides hold the surface to 0.00 in all cases; undo restores the baseline voxel count exactly.
- `componentVoxelScale` factored out of the Paint panel and shared with Sculpt's world-size readout.

#### Extrude (not a brush)
- Click a face, drag, and that whole face moves. `gatherFaceRegion` is a BFS **within the face plane**
  over voxels that are solid, have that same face **exposed**, and (under the Material scope) share the
  clicked voxel's **palette slot** — not colour, same reasoning as the paint fill. 4-connected, again
  like the fill: diagonal connectivity leaks across the corner where two faces touch.
- Extruded voxels take **their own source voxel's material**, not the palette's current entry —
  `extrudeFaceColors` is per face voxel, which matters under the Everything scope where a face spans
  materials and one colour would flatten the pattern.
- Extrude has no Mode, no size and no Place distance, and the panel **omits** them rather than greying
  them out (see the UI note below).
- **Needs no solidity override**, unlike the brush: the face and the axis are fixed on the press, and
  depth comes from `closestPointOnAxis` (the transform gizmo's own projection) rather than from a ray
  cast into the geometry. After the press it never asks the geometry anything, so the geometry it
  creates was never in a position to mislead it. `processSculptSample` returns before the pick.
- `extrudeAppliedDepth` is signed and is the whole committed state; each frame walks it toward the
  cursor **one layer at a time**, which is what makes a drag reversible in place.
- **Each layer records what it displaced**, split into `addedCoords` (were empty → reverse by removing)
  and `restoreCoords`/`restoreColors` (held something → reverse by writing it back). The naive version
  — retracting empties the cells the layer filled — deletes pre-existing geometry whenever a face is
  extruded *into* something already there (a stair tread into the tread above), and undo would too.
  **Found by reading the retract path, not by observing a failure**, which is why the test below builds
  the case deliberately.
- `runExtrudeSelfTest` (`EDITOR_SCULPTTEST=1`): the gathered face is checked to be one plane / one
  material / all-exposed; depth 1 must add exactly one voxel per face voxel; out-to-5-and-back and
  in-to-(-3)-and-back must return the voxel count to baseline **mid-drag**, before undo is involved;
  and undo of a real drag must too. The last check **parks a voxel of another colour two layers out**
  and verifies it survives with its own material — no natural face in any shipped scene extrudes into
  occupied space (every layer lands in empty air), so without this the displacement path is untested.
#### SelectionScope, and the Fill split
- `SelectionScope { Material, Everything }` is shared by **three** spreads: the Extrude face gather and
  both paint fills. They all answer "does a change of material stop this?", so they answer it once. The
  second label is supplied per call site ("Whole face" / "Whole volume") since what is spread over
  differs.
- Face size is wildly scene-dependent and that is why it has to be a toggle rather than a default —
  the numbers are not close:

  | Scene | `Material` face | `Everything` face |
  |-------|-----------------|-------------------|
  | StonehillCastle (flat-coloured) | 3,213 | 4,101 |
  | Sibenik (photo-textured) | 1 | 35,431 |
  | LostEmpire (photo-textured) | 4 | 31,963 |

  A photo-textured voxelisation gives nearly every voxel its own palette entry, so Material selects a
  handful and Everything is the only usable setting; on flat-coloured geometry Material is the precise
  one. Neither is right for both.
- **`PaintShape::Fill` was split into `FillFace` and `FillVolume`.** Adding the scope toggle to the old
  single Fill would have meant `Fill + Everything` = flood every connected voxel *including the
  interior* — one click repainting an entire model, reachable by a single toggle on the mode people
  already use for surfaces. The split makes the 3D case something you opt into: `FillFace` is
  `gatherFaceRegion` (surface-confined, cannot reach around a corner or inside), `FillVolume` is the
  old 6-connected flood. `collectPaintTargets` gained a `seedFaceNormal` parameter, which only
  `FillFace` uses; zero (a ray that began inside geometry) means no face and it collects nothing.
- `runFillSelfTest` now also gathers a **face** fill from the same seed and asserts it never reaches a
  voxel the volume fill did not and never leaves its plane — a face fill that escaped its plane would
  be a volume fill wearing the safer name, which is the exact surprise the split exists to prevent.

#### UI note (applies beyond Sculpt)
- **A setting the current brush/shape cannot use is removed from the panel, not disabled.** A greyed
  control still reads as part of the tool and still has to be ruled out; these panels are short enough
  that their shape changing is easier to follow than a column of dead rows. Trailing explanatory prose
  was cut back to labels at the same time — the Tool panel is an identifier for what you are doing,
  not a tutorial.

### What's Next (Phase 7+)
- Removal of legacy `chunkQueue`, mutability policies, persistence.


<!-- headroom:rtk-instructions -->
# RTK (Rust Token Killer) - Token-Optimized Commands

When running shell commands, **always prefix with `rtk`**. This reduces context
usage by 60-90% with zero behavior change. If rtk has no filter for a command,
it passes through unchanged — so it is always safe to use.

## Key Commands
```bash
# Git (59-80% savings)
rtk git status          rtk git diff            rtk git log

# Files & Search (60-75% savings)
rtk ls <path>           rtk read <file>         rtk grep <pattern>
rtk find <pattern>      rtk diff <file>

# Test (90-99% savings) — shows failures only
rtk pytest tests/       rtk cargo test          rtk test <cmd>

# Build & Lint (80-90% savings) — shows errors only
rtk tsc                 rtk lint                rtk cargo build
rtk prettier --check    rtk mypy                rtk ruff check

# Analysis (70-90% savings)
rtk err <cmd>           rtk log <file>          rtk json <file>
rtk summary <cmd>       rtk deps                rtk env

# GitHub (26-87% savings)
rtk gh pr view <n>      rtk gh run list         rtk gh issue list

# Infrastructure (85% savings)
rtk docker ps           rtk kubectl get         rtk docker logs <c>

# Package managers (70-90% savings)
rtk pip list            rtk pnpm install        rtk npm run <script>
```

## Rules
- In command chains, prefix each segment: `rtk git add . && rtk git commit -m "msg"`
- For debugging, use raw command without rtk prefix
- `rtk proxy <cmd>` runs command without filtering but tracks usage
<!-- /headroom:rtk-instructions -->

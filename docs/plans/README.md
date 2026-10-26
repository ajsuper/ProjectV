# Plans

Design notes, delivery logs and living lists. **Plans live here, in the repo, and are kept
current as work lands.** When you build from one, update its status line, and say where the
build differed.

Status: **proposal** (not agreed) · **approved** (not started) · **in progress** · **built** ·
**superseded** · **log** (a record) · **living** (kept current).

## Living documents

| Plan | Status | What |
|---|---|---|
| [known-latent-issues.md](known-latent-issues.md) | living | Real defects not currently triggered, and bundled assets that no longer load |
| [test-suite-notes.md](test-suite-notes.md) | living | What a test suite should cover, with the confirmed failure mode for each entry |
| [engine-promotion-candidates.md](engine-promotion-candidates.md) | in progress | Which scene-editor pieces should move into the engine; items 1–8 ruled on, 9–20 not |

## Runtime and physics

| Plan | Status | What |
|---|---|---|
| [physics-jolt.md](physics-jolt.md) | in progress | Jolt physics from voxels, deterministic and networkable. M0–M6, joints, materials, fracture, debris and support built; fracture policy next |
| [cheap-debris.md](cheap-debris.md) | built | A break's small pieces are not bodies: drawn, falling and bouncing against the static world |
| [impact-loads.md](impact-loads.md) | in progress | What decides a break and how big it is: energy, load share, ragged rims. How big a break a big slow body buys is open |
| [voxel-collision-shape.md](voxel-collision-shape.md) | Phases 1–3 built | Bodies collide as their tile tables, read in place. Phase 4 (the default) not started |
| [teardown-physics.md](teardown-physics.md) | draft | Converted Teardown levels as physics scenes: props, holes, joints |
| [world-loading.md](world-loading.md) | built (L0–L3) | One rule decides what is loaded: blocks, entities, physics under loading |
| [networking.md](networking.md) | built (N0–N7) | Server-authoritative on the deterministic simulation: transport, replication, prediction, interest, a hundred clients |
| [runtime-spine-entt.md](runtime-spine-entt.md) | built (P1–P4) | EnTT registry, stages and scheduler, `Time`, `Events`, the Scene bridge. P5 unscheduled |
| [authored-entities.md](authored-entities.md) | built | Gameplay data saved as ECS components in `entities.json` |
| [component-attachments.md](component-attachments.md) | built (AT0–AT2) | Program data that travels with components in `compose.json`; narrowed by authored-entities |
| [slot-reuse.md](slot-reuse.md) | in progress | Generational components, recycled chunk rows, indexed roots. Steps 1–3 and 5 built; the editor conversion (4) open |
| [animation.md](animation.md) | approved | Living scenes: per-voxel detail motion, envelopes, motion sets. Design agreed |
| [voxel-storage-tiles.md](voxel-storage-tiles.md) | in progress | Tile tables replace the tree64 as a blob's voxels: render, edit, simulate, store. Phases 1–5 built; what is left is in its delivery log |

## Editing

The editing system was delivered in phases. [edit-phases-changelog.md](edit-phases-changelog.md)
(log) records what each delivered.

| Plan | Status | What |
|---|---|---|
| [edit.md](edit.md) | built | Phase 1: per-component edit queues, copy-on-write blobs, negative grid expansion |
| [edit_p5.md](edit_p5.md) | built | Incremental GPU upload (`flushSceneUpdates`, `RangeAllocator`) |
| [edit_p6.md](edit_p6.md) | built | Hierarchical component tree, identity, transform API |
| [edit_p7.md](edit_p7.md) | not built | `EditingPolicy`, per-component drain, `SceneDiff`. None of these exist in the code |
| [edit_p8.md](edit_p8.md) | partly built | Writing a Scene back to disk (`saveComposeToDisk`) is built; per-mutability write-back is not |
| [edit_p9.md](edit_p9.md) | not built | Direct tree64 edits, transactions, factory functions |
| [edit_p10.md](edit_p10.md) | built | One material byte per voxel and per-component palettes, replacing `voxelTypeData` |

## Scene editor tools

Where a plan and [`examples/60-scene-editor/README.md`](../../examples/60-scene-editor/README.md)
differ, the README is what the code does.

| Plan | Status | What |
|---|---|---|
| [asset_model.md](asset_model.md) | built (stages 1–4) | One container all the way down: the open asset is the stack |
| [assembly_system.md](assembly_system.md) | built; container half superseded by asset_model | Constructive solid modelling: the fold, ops, bake |
| [stamp_system.md](stamp_system.md) | superseded by assembly_system | Shape placement and region copy/move |
| [scene_composition.md](scene_composition.md) | built (stages 1–3) | Booleans and snapping between finished `.data`s; stage 4 designed |
| [brush_system.md](brush_system.md) | built (stages 1–3) | The Brush Lab: Lua brushes, material roles, the Custom tool |
| [symmetry_system.md](symmetry_system.md) | built (stage 1) | Mirrored editing on a lattice; stages 2+ design only |

## Rendering

| Plan | Status | What |
|---|---|---|
| [render-target-sizing.md](render-target-sizing.md) | built | Render targets sized by rule (`sizeMode`), per-pass sizes |
| [loose-chunk-bvh.md](loose-chunk-bvh.md) | partly built (H1, H3, H5) | A BVH over loose chunks for rendering, picking and physics |
| [block-table-traversal.md](block-table-traversal.md) | done | Walking the tree64 without descending it (the dense block table) |
| [voxel-renderer.md](voxel-renderer.md) | in progress (R0–R3, R5 built; R4, R6 open) | The `_demo` examples' renderer: rebuilt for speed, then feel, then looks |
| [restir-pt.md](restir-pt.md) | delivered in part | ReSTIR PT: paths with materials, reused. Replaced `restir` in the gallery and the terrain generator |
| [color-convention.md](color-convention.md) | K0–K2 built | One meaning for a stored colour (sRGB), one transform to the screen |
| [region_tree64_layout.md](region_tree64_layout.md) | proposal | Column-based region allocator for the tree64 texture |
| [shader_optimization/](shader_optimization/README.md) | stale | Twelve independent DDA shader experiments, written against the pre-Phase-10 shader (`voxelTypeData`, the old `docs/examples/` paths) |

The twelve shader experiments:
[01 subtree-empty flag](shader_optimization/01_subtree_empty_flag.md) ·
[02 coarse cellMap mip](shader_optimization/02_coarse_cellmap_mip.md) ·
[03 redundant headers](shader_optimization/03_redundant_headers.md) ·
[04 MOVE_LUT arithmetic](shader_optimization/04_move_lut_arithmetic.md) ·
[05 direct voxelType address](shader_optimization/05_voxeltype_direct_address.md) ·
[06 voxelType fallback fix](shader_optimization/06_fix_voxeltype_fallback.md) ·
[07 forward mask skip](shader_optimization/07_forward_mask_skip.md) ·
[08 cache node data](shader_optimization/08_cache_node_data.md) ·
[09 maxDistance early exit](shader_optimization/09_maxdistance_early_exit.md) ·
[10 root empty check](shader_optimization/10_root_empty_check.md) ·
[11 single-child fast path](shader_optimization/11_single_child_fastpath.md) ·
[12 cache and prefetch](shader_optimization/12_cache_and_prefetch.md)

## Content

| Plan | Status | What |
|---|---|---|
| [biome-canyonlands.md](biome-canyonlands.md) | approved; C1, C2 built | The terrain generator's first biome: a Colorado Plateau canyon network |

## Infrastructure

| Plan | Status | What |
|---|---|---|
| [logging.md](logging.md) | built | Compile-time log categories (`PROJV_LOG_*`, `[TRC]` `[PRF]` ... tags) |

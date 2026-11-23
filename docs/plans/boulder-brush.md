# Boulder brush

Status: **built** (B1). The scene editor's second object generator, beside the tree brush: boulders of
many shapes, rocks, colours and sizes, alone or in groups, with moss and lichen.
`examples/60-scene-editor/brushes/boulders.lua`.

`rocks.lua` stays what it is -- the canyonlands biome's three named sandstones, baked by
`terrain_asset_baker`. This is the general one.

## Decisions

| # | Decision | Why |
|---|---|---|
| D1 | A new brush, not more kinds in `rocks.lua` | `rocks.lua` is a biome's assets, baked from `designs.json`; its palette and look are pinned by that. A general brush wants its own palette and settings. |
| D2 | **Shape and rock are separate axes.** Six shapes (tick any: rounded, angular, slab, split, tor, columns) x eight rocks (granite, basalt, sandstone, limestone, slate, gneiss, ironstone, quartzite, or Mixed) | Variety comes from the product. A split granite boulder and a split sandstone one are the same fracture in two rocks. |
| D3 | A rock is a colour **and a relief**: its texture is partly geometry (vesicles, tafoni, karren runnels, foliation ridges, knobs) added to the distance near the surface | Paint alone reads flat under real light; the renderers shade voxel relief. |
| D4 | A **group is one placement**: one call returns every boulder, the rubble and the moss | The host's fit test is all or nothing per placement, so a group lands whole or not at all, and undo takes it back whole. |
| D5 | Boulders are **dropped**: each rests on the ground under its own footprint (found with `pv.solid`), or on the boulders placed before it, then sinks by `Sunk` | Groups on uneven ground, and piles that lean on each other, without floating or half-buried-in-a-wall placements. Cells at or below the ground are clipped rather than written, so nothing lands in occupied terrain. |
| D6 | Moss is computed **after** the group is assembled, from the union | Moss belongs on tops, the shaded (-z) side and low down near the ground, and the union knows which faces are open. Dense moss is a cushion of extra voxels, not paint only. |
| D7 | Moss is named `moss.*` | The tree brush's undergrowth list already grows through `moss.`, so trees can stand beside mossy rocks. |
| D8 | Each declared role has a colour unlike every other's | Interning falls back to matching by colour; a duplicate would bind to the wrong entry. |
| D9 | Too big for one placement (110k voxels): estimate each boulder's surface first and refuse at once; the interior is kept as runs per column and written only if the group fits, else it is a shell | The host stops any call past 2 s. Storing the interior cell by cell timed out at Height 64 and Width 2.5; the estimate makes the refusal immediate and names the settings that fix it. |
| D10 | Gloss and metal per role: each rock's ramp has its own (slate satin, quartzite glossy, ironstone a metal lustre), and the accents are roles of their own (mica, pyrite, haematite, quartz) | A ramp shares one finish, so a glint has to be its own role. *Minerals* sets how many there are. |
| D11 | No sort: the output is built in a fixed order (boulder, column, cell) | A Lua comparator sort cost about half a second at 100k voxels; a fixed build order gives the same determinism. |

## Cost

71 palette roles (the tree brush: 63), all interned on the first stroke. A component near its
255-entry ceiling refuses the brush with the editor's usual message.

## Known limits

- `pv.solid` cannot tell grass from ground, so a boulder planted in tall grass rests on the blade
  tips. It displaces the blades it overlaps, but the base can stand a few voxels above the soil.
- Moss cannot spread onto the ground around a boulder: a scatter brush only writes empty cells.

## As built

B1, 2026-11-23: the brush, its README section, and this plan.

Measured through the editor's brush host (`brush.cpp`) on flat and sloped ground, with Force on:
a single boulder of Height 30 takes 70-450 ms; six at Height 24, about 0.5 s; the slowest accepted
case, four at Height 48 with rubble, about 0.9 s. No voxel lands in the ground on a slope, and the
same site gives a byte-identical placement. Not yet seen in the editor's renderer.

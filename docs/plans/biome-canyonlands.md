# Biome 1: Canyonlands (Colorado Plateau)

Status: **approved**, 2026-10-23 (compressed scale). **C1, C2 built; C3 in progress** (2026-10-23). Replaces the "Perlin hills + conifers + 3D rock noise" terrain in
`examples/50-terrain-generator`, which was tuned by eye and never converged. This is the first of
several biomes; the structure below (a biome is data + a few functions) is what later biomes and the
blend between them build on.

## Why this biome

| Reason | |
|---|---|
| The rock is layered by nature | A voxel stair-step is what a bedded cliff looks like; it works with the grid, not against it |
| Distinct face types | Sheer walls, ledges, domes, banded slopes, talus: each a separate, testable problem (F1-F5) |
| Overhangs are structural | Alcoves and caprock overhangs come from *which layer is hard*, not from noise strength |
| Sparse vegetation | Small, scattered trees: cheap, and each one is seen, so asset quality shows |
| Strong colour | Red, maroon, lavender and cream bands, dark varnish streaks: read at any distance |

Reference parks: Capitol Reef, Canyonlands (Island in the Sky), Zion, Arches.

## Research summary

**Stratigraphy** (bottom to top, the Glen Canyon group over the Chinle). Slope figures from the Bergen
weathering theses ([1], [2]); colours from NPS Capitol Reef ([3]) and [4].

| Unit | Forms | Weathering slope | Colour | Hardness (relative) |
|---|---|---|---|---|
| Chinle Fm. | Banded slope, scree-covered | ~30 deg | Lavender, red, blue-grey, green-grey bands | Soft (mudstone) |
| Wingate Ss. | **Sheer wall**, vertical jointing | 77-90 deg | Deep red-orange to red-purple, black varnish streaks | Hard, but survives only under a cap |
| Kayenta Fm. | **Ledges and benches**, the cap on the Wingate | 65-70 deg overall | Darkest band: red-brown, maroon, lavender | Hardest per bed; interbedded with soft shale |
| Navajo Ss. | **Domes**, slickrock, alcoves | 43-48 deg, rounded | White/cream to tan, salmon cross-bed bands | Medium; massive, cross-bedded |

- Cliff vs slope is set by the sandstone/mudstone ratio ([1]). A wall holds its shape where a harder
  layer caps it; the soft layer below it is undercut, the cap fails, the face retreats ([5], [6]).
  **Overhangs and alcoves live at the base of a hard layer sitting on a soft one.**
- Cliff retreat drops blocks of the cliff-former onto the slope below: talus aprons of angular Wingate
  blocks on the Chinle slope ([6]).
- **Desert varnish:** dark (Mn/Fe) vertical streaks running down walls from where water pours over the
  rim ([7]).
- **Navajo:** cross-bedding (dune laminae, curved, dipping) etched into relief and colour; weathering
  pits on flat tops; arched alcoves in walls ([7], [8]).

**Vegetation**: pinyon-juniper woodland ([9], [10], [11]).

| Species | Form | Height | Where |
|---|---|---|---|
| Utah juniper | Bushy, rounded or irregular crown; trunk forked low and **twisted**; shredded strip bark | 3-8 m | Everywhere on benches and slopes, the most common tree; lower ground |
| Two-needle pinyon | Low, bushy, irregularly rounded spreading crown, denser and greener | 4-10 m (here) | Higher benches, with juniper |
| Dead juniper (snag) | Silver-grey twisted wood, durable, stands for centuries | 2-6 m | Scattered, ~1 in 8 |
| Fremont cottonwood | Broadleaf, wide crown, bright yellow-green | 12-20 m | Only in canyon-floor washes (stretch) |
| Sagebrush / blackbrush | Grey-green shrub | 0.5-1.5 m | Understory, sparser under trees |

Canopy is open: 10-60% cover ([10]), and understory thins as tree density rises.

**Terrain technique** ([12], [13]): a mesa is a heightfield plus per-layer hardness, with flat caps
on the hard layers; real erosion simulation is too slow to stream. The approach below generalises
"hardness per layer" into a **3D profile**, which is what gives overhangs.

## Scale

Strata are compressed against the trees, as games do (a real Wingate wall is ~100 m, ~15x a juniper).

| | Voxels (nearest ring, 2.8 units each) |
|---|---|
| Juniper | 35-55 |
| Pinyon | 50-75 |
| Cottonwood | 90-110 |
| Shrub | 4-10 |
| Chinle slope / Wingate wall / Kayenta / Navajo | ~60 / ~150 / ~50 / ~150: ~410 total, the current relief budget |
| Boulders | 4-40 across |

## The terrain model

One 2D field and one 1D function, so every face type falls out of the stratigraphy instead of noise:

```
  e(x,z)  "how far into the plateau": distance from the drainage network, in world units
          (domain-warped canyon network; 0 in a wash, large on a mesa top)

  w(y)    the profile: how far back from the drainage the rock face stands at height y.
          Built by walking up the strata: each layer adds  thickness / tan(slopeAngle)  of setback,
          so a 30-deg Chinle slope is wide, an 85-deg Wingate wall is nearly zero width.

  solid(x,y,z)  <=>  e(x,z) + detail(x,y,z) > w(y)
```

```
 y ^        Navajo dome (rounded: w grows smoothly)
   |      .-''''-.
   |     /        \___ Kayenta bench (ledge: w steps out, then back)
   |    |  <alcove      ______
   |    |   Wingate wall  (w almost constant -> vertical)
   |     \___undercut (w(y) dips at the Wingate's base: overhang)
   |         \___ Chinle slope (w grows fast -> 30 deg) + talus
   +---------------------------------------------> e (distance from wash)
```

- `w(y)` need not be monotonic: where it decreases with height the rock overhangs. Alcoves and the
  undercut under a cap are a dip in `w` at a soft layer's top, varied along the face by noise.
- `detail` is per face type (F1-F5), small against a layer's setback, and keyed on the unit at y.
- Mesas and buttes: where `e` has a local maximum smaller than the full profile's width, the top
  layers are missing and the landform is a butte capped by whichever layer is left.
- Ground cover (sand, soil, slickrock) and plant placement read the unit at the surface and the slope.
- Same streaming rules as now: a pure function of world position, chunk-seam safe, bounds from the
  profile's extremes, per-voxel work only in the band around the surface.

## Face types

Each is built and judged alone, in a lab view (below), before the terrain uses it.

| # | Face | Geometry (`detail`) | Colour |
|---|---|---|---|
| F1 | Wingate wall | Planar slabs bounded by vertical joints (spacing 10-25 voxels); slabs set back 0-3 voxels as units; rectangular spall scars | Red-orange ramp, per-slab tone; **varnish streaks**: dark, from the rim down, where water leaves the cap |
| F2 | Kayenta ledges | Thin beds alternating hard (stand out 1-3 voxels, flat tops) and soft (recessed, crumbly) | Maroon / red-brown / lavender by bed |
| F3 | Navajo dome / slickrock | Smooth, convex; cross-bed laminae as faint relief; pits on flat tops; arched alcoves in steep faces | Cream to tan, salmon bands following the cross-beds (curved, dipping), not horizontal |
| F4 | Chinle slope | Smooth 30-deg slope with rills (gullies down the fall line) | Horizontal bands: lavender, red, blue-grey, green-grey |
| F5 | Talus / boulder apron | Scattered boulder assets (B1-B3) at the foot of walls, densest right under them | From the cliff-former above |

## Assets

All baked, current compose format, in `examples/50-terrain-generator/biomes/canyonlands/`:

| # | Asset | How |
|---|---|---|
| T1 | Utah juniper, 4-5 variants | Scene editor tree brush, a new juniper mode: low fork, twisted stems, clumpy irregular foliage, strip-bark trunk colours |
| T2 | Pinyon, 3-4 variants | Tree brush: rounded bushy crown |
| T3 | Dead juniper snag, 2 | Tree brush, bare, silver-grey |
| T4 | Sagebrush shrub, 3 | Tree brush or a small shrub mode |
| T5 | Cottonwood, 2 (stretch) | Tree brush, broadleaf spreading |
| B1 | Wingate block, 6+ | New `rocks.lua` brush: jointed box, chipped edges, varnish on top-facing faces |
| B2 | Navajo boulder, 4 | `rocks.lua`: rounded, cross-bed bands |
| B3 | Kayenta slab, 3 | `rocks.lua`: flat plate |

`terrain_tree_baker` became `terrain_asset_baker`, a general asset baker (designs list per biome; brush per design).
The current conifer set moves to a future alpine biome folder rather than being deleted.

## Stages

Each ends with captures (own-app bgfx, `TERRAIN_CAPTURE`) shown to the user, and a commit once
approved.

| # | Stage | Done when |
|---|---|---|
| C0 | This plan | Approved |
| C1 | Assets: T1-T4, B1-B3; lab `TERRAIN_LAB=assets` shows them in a row on flat ground | Each reads as its species / rock at a glance |
| C2 | Face types F1-F4 as isolated 200x200 wall samples; `TERRAIN_LAB=faces` | Each face recognisable against reference photos |
| C3 | Terrain shape: `e`, `w(y)`, mesas, buttes, washes, flat-shaded per unit; `TERRAIN_LAB=shape` | Reads as canyon country from above and from the floor |
| C4 | Combine: faces on the shape, F5 talus, ground cover, vegetation placement | Full scene captures, fps not below the current terrain |
| C5 | Biome interface: the biome as a struct (strata, profile, faces, species, ground) so a second biome is new data, not new code | A stub second biome compiles against it |

**C1 as built.** Tree brush (`60-scene-editor/brushes/trees.lua`) gains Juniper, Pinyon, Dead juniper
and Sagebrush kinds: the broadleaf's space colonization with a per-kind `look` (envelope shape and lobes,
twist, strip-bark stripes, dieback of whole main limbs to silver wood, berries, foliage ramp and fill),
plus a small shrub generator. New `rocks.lua`: Wingate block, Navajo boulder, Kayenta slab, each a signed
distance coloured as albedo (varnish streaks, cross-bed bands, per-bed tones, lichen patches). 26 assets in
`biomes/canyonlands/designs.json`, baked by `terrain_asset_baker`; the conifers moved to `biomes/alpine/`.
Lab: `TERRAIN_LAB=assets`.

**C2 so far.** `include/canyonlands.hpp`: the setback model (`e > w(x, y)`) for Wingate and Kayenta,
heightfields of `e` for Chinle and Navajo. Built as one cross-section rather than four isolated samples,
so the junctions are judged too (`TERRAIN_LAB=faces`). Review notes folded in: Chinle colours warmed and
mostly buried in wall debris; gullies start at pour-off chutes in the wall; joints wander (two noise
octaves) and fork into subtle slanting branch cracks. Wingate blocks sharpened (fewer, cleaner break
planes); stamps can turn and mirror. Second review: the wall gets bulges and recesses built of whole slabs (a
coherent field sampled per slab, so faces stay planar; overhangs where it changes with height) and arched
alcoves; Chinle debris as Worley fragments, one shade each; the Navajo is one mass rising off a bench of
varying width (none to 70 units), broken into domes at two scales with only some edges cut to clefts.

**C3 so far.** The canyon terrain is the terrain generator's default (`TERRAIN_BIOME=alpine` for the old
hills). `e` is `canyonlands::drainageDistance`: Worley edges on warped coordinates, (f2 - f1) * cell / 2, at
three scales each cut to its own depth -- main canyons to the wash (cell 1700), masked box canyons floored on
the Chinle (650), masked Navajo clefts down to the bench (260). The strata rise and fall +-150 units over
~3.6 km (`strataBase`). Walls of any direction take one of two perpendicular joint sets, by e's gradient
(`wallAlong`). Junipers, pinyons and snags placed by the existing placer; boulders and shrubs wait for C4.
Camera presets: `TERRAIN_CAMERA=cliff`, `TERRAIN_CAMERA=canyon`.

**C3, second pass (2026-10-24).** The Worley network read as fragments: every canyon a cell edge, every
mesa the same size. Replaced by a local Dendry-style hierarchy (Gaillard et al., I3D 2019): jittered
points at 2400 / 900 / 330 units; trunks join the lowest neighbour on a low-frequency control field,
tributaries the nearest trunk, side canyons the nearest coarser segment; depth and width by level, trunks
widening by a regional maturity field (young gorge .. wide valley). Basins where the control field is
lowest, with buttes in them. A global flow simulation was considered and rejected: it would tie every
point to the whole region (no independent chunks, no infinite world, hard to blend with other biomes).
Segments are collected per 1024-unit tile and binned by reach (e is only needed to ~480 units), in a
per-thread memo -- a cache, not state. Also: strata tilt (a monocline) and thicken by region; the floor has
dune fields, rock pavement and soil, with vegetation in clumps on the soil; pebble-scale relief and specks.

Open: nearest-ring chunks take ~1 s to generate (the voxel pass fills each mesa column from its top down
to the Chinle); fill only near exposed surfaces.

Later, not now: more biomes, and blending (per-column biome weights from a Voronoi biome map with
soft borders; blend `e`, profiles and scatter by weight).

## Open questions

- Scale above: strata compressed ~15x against trees. Alternative: bigger trees, less relief.
- Cottonwood washes: stretch, after C4.
- Hoodoos / arches (Entrada): not in this biome's first pass.

## Sources

1. Univ. of Bergen, weathering of arid continental deposits, Colorado Plateau: https://bora.uib.no/handle/1956/10407
2. Univ. of Bergen thesis (slope angles by unit): https://bora.uib.no/handle/1956/10326
3. NPS Capitol Reef, geology big picture: https://www.nps.gov/care/planyourvisit/upload/Geology-big-pictureDec15-accessible.pdf
4. Kayenta Formation: https://en.wikipedia.org/wiki/Kayenta_Formation
5. Scarp retreat: https://en.wikipedia.org/wiki/Scarp_retreat
6. Hoodoo (geology): https://en.wikipedia.org/wiki/Hoodoo_(geology)
7. NPS Geodiversity Atlas, Navajo NM: https://www.nps.gov/articles/nps-geodiversity-atlas-navajo-national-monument-arizona.htm
8. Utah Geological Survey, Navajo Sandstone: https://geology.utah.gov/tag/jurassic-age-navajo-sandstone/
9. NPS, pinyon-juniper woodlands: https://www.nps.gov/articles/pinyon-juniper-woodlands-species-composition-classification.htm
10. USGS NVCS association (canopy cover, understory): https://www1.usgs.gov/csas/nvcs/unitDetails/1085061
11. USU Extension, Utah juniper: https://extension.usu.edu/rangeplants/shrubs-and-trees/juniper-utah.php ; NPS Arches: https://www.nps.gov/arch/learn/nature/cupressaceae_juniperus_osteosperma.htm
12. Procedural table-mountain modelling (Applied Sciences 2019): https://www.mdpi.com/2076-3417/9/11/2352/html
13. PCG Mesa Landscape: https://github.com/TeoNikolov/PCG-Mesa-Landscape

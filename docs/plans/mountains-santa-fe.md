# Mountains: the Sangre de Cristo at Santa Fe

Status: **approved**, 2026-11-21. **M1-M3 built, M5 two passes, M7 built** (D2 revised: a smooth far field); M4 (the local scale) next. A new example, `examples/55-mountains` (target
`mountains`), that generates mountains in the style of those around Santa Fe and draws them with a fast
renderer of its own: voxels all the way to a horizon about 100 km out, detailed underfoot. It leaves
`50-terrain-generator` (canyonlands, alpine) as it is.

Agreed with the user (2026-11-21): new example; **voxels to the horizon** (no heightfield or impostor
far field); fast direct light, no path tracing; *in the style of* Santa Fe (procedural, seedable,
infinite), not real elevation data.

## What has to be true

| # | Goal | Check |
|---|---|---|
| G1 | Reads as the Sangres from Santa Fe: a piedmont ramp, foothills, a long crest 1.5 km above town, other ranges on the horizon | Side by side with reference photos, judged by the user |
| G2 | Sells scale: a range 60-70 km off is visibly far (aerial perspective), and nothing pops or shimmers as it scrolls | Fly captures; still-camera captures with no flicker |
| G3 | Detailed underfoot: rocks, soil, junipers and pinyons at about 0.25 m a voxel | Walk captures |
| G4 | Fast | Flying at 300 m/s, the horizon keeps up. >= 60 fps at 1440p on a discrete GPU (RTX 4070 class; to be measured there). This machine is an integrated Radeon 860M: playable, ~50 fps at 1600 x 900 (as built, M3) |
| G5 | Infinite and seedable; a pure function of position, so chunks generate on any thread in any order | `MOUNTAINS_SEED`, seam tests |

## Reference (research, 2026-11-21)

Labels: [S] sourced, [D] derived from sourced numbers, [U] uncertain (check against a 10 m USGS DEM if it
matters).

| Feature | Elevation | From the Plaza | |
|---|---|---|---|
| Santa Fe Plaza | 2,133 m | -- | [S] 1 |
| Atalaya Mtn (first foothill) | 2,781 m | ~6 km E | [S] 2 |
| Tesuque Peak / Lake Peak / Santa Fe Baldy | 3,671 / 3,782 / 3,850 m | 17 / 19 / 23 km ENE-NE | [S] 5, [D] |
| Ski basin base | 3,155 m | -- | [S] 4 |
| Treeline | ~3,500-3,600 m | -- | [D] 6, [S] 23 |
| Redondo Peak (Valles Caldera dome; caldera 22 km across) | 3,431 m | ~60 km WNW | [S] 7 |
| Sandia Crest (west face 1,200 m in ~3 km) | 3,255 m | ~70 km SW | [S] 8 |
| Ortiz Mtns | 2,712 m | ~40 km SSW | [S] 9 |
| Caja del Rio basalt mesa; cinder cones to 240 m | ~2,200-2,280 m | 15-25 km W-SW | [S] 10 |
| La Bajada escarpment (basalt-capped) | ~180 m tall | ~29 km SW | [S] 11 |

**Landforms.**
- The front is a gentle piedmont ramp, ~5 degrees on average from town to the crest [D]. It steepens into
  foothills at ~2,300-2,400 m. Canyon sides are 25-35 degrees and talus ~35 degrees [U].
- Drainages (Santa Fe River, Little Tesuque, Tesuque, Rio en Medio, Nambe) run west off a N-S crest.
  They are roughly parallel, 4-7 km apart at the front, and dendritic upstream [U].
- Summits are rounded grassy alpine domes. The sharp relief is in glacial cirques under them (Nambe Lake
  under Lake Peak) [S] 5, 13, 14.
- The core is pink to red Proterozoic granite with grey gneiss and schist screens [S] 3, 12.
- The basin is soft pinkish-tan Tesuque Fm.: ledgy, dipping ~10 degrees W, with badlands (Pojoaque
  Bluffs) [S] 15-17.
- Dark basalt caps flat mesas that end in sharp scarps, with cinder cones [S] 10, 11.

**Vegetation**, by elevation (aspect-controlled: north slopes fir, south slopes pine):

| Band | Elevation | Look |
|---|---|---|
| Pinyon-juniper | ~1,950-2,300 m [U] | 10-30% cover over bare tan soil: the **dotted hillside** [S] 18, 19 |
| Ponderosa (+ PJ) | ~2,100-2,600 m | Open, tall, orange bark [S] 20 |
| Mixed conifer (Douglas fir, white fir) | ~2,400-3,000 m | Dark, closed on north slopes [S] 20, 21 |
| Spruce-fir + aspen | ~2,900-3,500 m | Dark blue-green; aspen patches (gold Sept-Oct, bare grey-white in November) [S] 21, 22 |
| Alpine tundra | above ~3,550 m | Tan grass domes, talus |

**Light.** Visual range averages ~267 km at Wheeler Peak [S] 25, so ranges at 60-70 km stay crisp under a
thin blue haze. 78% of possible sunshine; a deep blue sky at 2,100 m. In late November, snow on north
faces and above ~3,000 m [U]. Alpenglow on snow at sunrise and sunset [S] 26.

## Decisions

| # | Decision | Why |
|---|---|---|
| D1 | **1 unit = 1 m**, finest voxel **0.25 m** | Real elevations and distances can be used as is (the island demo already uses metres). At 0.25 m a juniper is 12-30 voxels tall and a person 7, which is enough for G3. 0.125 m would quadruple the near ring's cost for detail at arm's length |
| D2 | **A clipmap of voxel rings**: ring k has voxels of 0.25*2^k m and a radius of N voxels, centred on the camera. Eleven rings (k = 0..10) reach ~100 km at N = 512 (ring 10: 256 m voxels, 131 km) | With a constant N, a voxel covers a near-constant number of pixels at every distance (~1.3 px at N = 1024, ~2.7 px at N = 512, at 1440p and 60 degrees). Memory is about the same per ring, so the cost grows with log(distance), not distance^2. N is measured in M2 (open question 1) |
| D3 | **One `SceneGrid` per ring, a hole where the finer ring is** (cells the inner ring covers are empty). The rings snap to their own cell size, so ring k+1's hole is whole cells | The engine already marches every grid and keeps the nearest hit (`pjv_scene.sc`, the grid loop), so nested rings need **no shader change** to start. Without holes, a coarse surface would poke through the fine one |
| D4 | **Rings scroll by whole cells**: when the camera crosses a cell of ring k, a strip of cells at the leading edge is generated and the trailing strip freed. Rebasing the grid is `originCellCoord` plus a rewrite of `cellToChunk`. If that rewrite and its upload are too slow at 11 rings, the fix is **toroidal cell addressing** in `SceneGrid` (engine change, its own milestone) | Each ring changes only in proportion to how far the camera moved in its own cells, so far rings almost never change |
| D5 | **The height is generated at the ring's resolution, with octaves finer than the voxel dropped** (prefiltered), and coarse rings never read finer ones | Coarse rings stay cheap: ring 10 is a few octaves per column. A coarse ring then shows the average of the fine one rather than an alias, so the seam between rings doesn't shimmer (G2) |
| D6 | **Terrain = a 2D landform height plus a 3D band near the surface, in the near rings only** (k <= 2: rock outcrops, ledges, talus blocks, cut banks) | Mountains at this scale are heightfields; overhangs only matter within ~0.5 km. The same rule as `50`: per-voxel work only in a band around the surface |
| D7 | **The landform comes from three layers.** (1) A **province map**: warped Voronoi regions, each a landform type (fault-block range, piedmont basin, volcanic dome/caldera, basalt mesa field, isolated hill mass) with its own uplift profile, so a crest 20 km off and other ranges 60-70 km off fall out of it. (2) **Ridge and valley structure**: drainages off the crest, from a local Dendry-style hierarchy (as in canyonlands C3) or the runevision erosion filter (gullies and ridges per octave, local, streamable); M1 picks between them by eye. (3) **Detail**: derivative-damped fBm (IQ) so slopes are rough and floors smooth | Every layer is a pure function of position (G5). A global erosion simulation (Mei 2007, Cordonnier 2016) gives better watersheds but ties every point to its whole region, as canyonlands found. Revisit only if M1's networks don't read |
| D8 | **Cirques and alpine domes are a rule of the range**: above ~3,400 m, rounded summits, with cirque bowls carved on the north and east sides of the high points (a bowl SDF subtracted, a tarn in its floor) | The look of the Baldy/Lake Peak skyline: soft domes with steep bitten-out headwalls |
| D9 | **Materials and vegetation from elevation, slope, aspect and the province.** Granite (pink-red) and gneiss (grey) on steep rock; tan-pink soil and grama on the piedmont; Tesuque Fm. bands in badlands; basalt on mesa caps; snow by elevation and aspect, with `MOUNTAINS_SEASON` (default late November) | Bands from the table above. The dotted pinyon-juniper slope is the single most recognisable thing about Santa Fe hills |
| D10 | **Trees at three levels of detail by ring**: stamped voxel assets (k <= 1); a canopy blob, a few voxels of the tree's colour on a trunk, (k = 2-4); only a tinted ground voxel beyond. Assets come from the editor's tree brush, which already makes juniper, pinyon and dead juniper (canyonlands C1); new kinds: ponderosa, Douglas fir, Engelmann spruce, aspen (bare and gold) | A juniper is 1-2 voxels at ring 3. Stamping full assets there would cost much and show nothing |
| D11 | **A renderer of its own, forked from the gallery's `fast`** (half resolution, rebuilt by face identity, TAA; ~2.8 ms at 1080p): one primary march, a **sun shadow ray** that starts in the ring the hit is in and walks outward, a sky term with a few short AO rays near and none far, a **physical sky with aerial perspective** (Hillaire 2020: transmittance, sky-view and aerial-perspective LUTs, density for 2.1 km altitude and dry air), and snow alpenglow from the same sun colour | Aerial perspective is what sells 60 km (G2): distant ridges stepping blue in layers. The shadow ray is the expensive part. Far hits keep it cheap by starting coarse |
| D12 | **Streaming reuses the shape of `50`'s pool** (priority by ring then distance, dedup, a result ring) but **lives in the example**. It is not moved into the engine here | `50/ENGINE_OPPORTUNITIES.md` already argues for an engine job system. That is its own plan, not a side effect of this one |
| D13 | **A 2D lab first**: `mountains_lab` writes a hillshaded relief map, an elevation-banded map and a profile along a line (town to crest) as PNGs in seconds | Landform is tuned in 2D where an iteration is seconds, not in 3D where it is a rebuild and a flight. That is how canyonlands found its network wasn't working |

```
  ring k:  voxel 0.25*2^k m, radius N voxels, one SceneGrid, hole where ring k-1 is

     k=10 ................................................ 131 km (N=512)
     k=9   ..................................... 66 km
     ...
     k=2  [  1 m voxels  [ k=1 0.5 m [k=0 0.25 m, 128 m]  ]  ]
                                       ^ camera
```

## Milestones

Each ends with captures shown to the user and a commit once approved.

| # | Milestone | Done when |
|---|---|---|
| M0 | This plan | Approved |
| M1 | **Landform in 2D** (`mountains_lab`): province map, crest, drainages (Dendry vs erosion filter, chosen by eye), piedmont, cirques, a mesa field, distant ranges. Hillshade, elevation-band and profile PNGs | Hillshade and profile read as Santa Fe to the Sangres; the user approves |
| M2 | **The clipmap** in a new example, `55-mountains`, drawn with the basic renderer: rings as grids with holes, prefiltered generation, scrolling, the pool. Measure N = 512 vs 1024 (memory, generation time per ring, frame time). Fly at 300 m/s | No gaps or cracks at ring seams; the horizon keeps up; numbers written into this plan |
| M2b | (only if M2 shows scrolling is too slow) **toroidal grid addressing** in the engine, with tests | Scrolling a ring costs only its new strip |
| M3 | **The renderer**: fork `fast`; physical sky and aerial perspective; ring-aware sun shadow; sky term and near AO | G4 measured; distant ranges blue and layered (user judges) |
| M4 | **Ground**: materials, the 3D near band (granite outcrops, talus, cut banks, badland ledges, basalt rims), snow | Walk and fly captures approved |
| M5 | **Vegetation**: the band table, aspect, the three tree LODs; new assets through the tree brush | The dotted PJ foothills and the dark fir slopes read from town |
| M6 | **Finish**: camera presets (`MOUNTAINS_CAMERA=plaza|atalaya|baldy|...`), `MOUNTAINS_CAPTURE`, a README, docs rows, a selftest that flies a fixed path headless-capable and checks seams | Docs ctest passes; the example is in `examples/README.md` |

## M1 as built (2026-11-21)

`examples/55-mountains/include/mountains/landform.hpp` (header-only, no engine types) and
`mountains_lab` (`tools/lab.cpp`; region, local, profile and two panoramas from the Plaza, ~25 s on 16
threads).

- **Macro**: a base that falls toward the Rio Grande (a polyline with elevations; the river's elevation is
  blended over segments, since taking the nearest one left creases), plus features: `Range` (crest along a
  line, rounded over 1.8 km, asymmetric concave flanks with a long apron, crest wander and peaks along it,
  flanks varying in width), `Dome` (shield, caldera with a noisy wall, resurgent dome), `HillMass`,
  `MesaField` (a level basalt cap over a talus apron, cinder cones). Laid out like Santa Fe within 95 km;
  one random feature per 50 km cell beyond.
- **Erosion**: phasor gullies (5x5 cells, a 1.5-cell falloff: a 4x4 neighbourhood missed points and left
  straight seams), 9 octaves from 4.5 km, each `0.42 * 0.62^i * wavelength * slope` tall and following the
  gradient so far. **Capped by relief**: the cap is `0.3 * relief * smoothstep(150, 700, relief) + 7`,
  falling 0.6x per octave, where relief is the macro height over the base. Without it the piedmont grew
  130 m hills 600 m from the Plaza and mesa scarps grew spikes.
- **Detail**: slope-damped fBm from 160 m to the footprint. **Cirques**: bowls on north- and east-facing
  sides under crests above 3,450 m, about half of them, some with tarns. **River gorge** cut last.
- Reference elevations (target / got): Plaza 2133 / 2185, Tesuque Pk 3671 / 3816, Baldy 3850 / 3846,
  Redondo 3431 / 3406, Sandia 3255 / 3193, Ortiz 2712 / ~2400, Cochiti 1620 / 1612.
- **Every ring computes the same erosion (D5).** Erosion is chaotic: each octave follows the slope the
  octaves before it left, so any difference in that slope moves the gullies (a lattice-interpolated
  sampler first disagreed with the point sampler by up to 240 m, at any lattice spacing). So the slope
  is part of the definition: octave k reads the surface so far from a global lattice of spacing
  wavelength_k / 8, Catmull-Rom (Hermite from stored slopes left lattice-aligned bumps wherever the
  slopes disagreed with the values), and heights are the exact macro plus erosion plus detail. A grid
  coarser than a lattice runs that octave per point, and differs by that octave alone. `sampleGrid` is
  the sampler; `sample` is a 1 x 1 grid. `mountains_lab --check`: a 64^2 grid against the next coarser
  one differs by 0.01 m at 0.25/0.5 m, 0.02 m at 2/4 m, 12 m at 16/32 m and 24 m at 128/256 m (max;
  rms 1-2 m), all well under the coarser voxel. 4-7 us a sample.
- Slopes tuned to a mean of 22 degrees across the range (erosion 0.32, gain 0.55; it was 35 with a
  quarter over 49 degrees). Rock from 40 degrees.
- `include/mountains/surface.hpp`: material classes (14 x 16 shades), the mix at a point from elevation,
  slope, aspect and the province, blended for the lab and dithered per voxel.
- Open: random provinces outside the layout read as isolated stamps (capsule ranges, round mesas). Basin
  and range chains, en-echelon ranges and dissected basins are later work; the Santa Fe layout is the
  priority.

## M2 as built (2026-11-21)

`examples/55-mountains`, target `mountains` (`main.cpp`, `include/mountains/columns.hpp`), drawn with the
engine's basic renderer.

- **Rings**: one `SceneGrid` and one component each (the same 224-slot palette interned into each). Ring k
  spans 2R cells (R = N / 64), snapped to every second cell; its hole is ring k-1's extent. A ring
  **generates its hole too, last, and keeps it**; a hole cell is hidden only once ring k-1 has generated
  the four columns over it. So nothing is ever missing: while a finer ring catches up, the coarse cells
  show. The grids are rebuilt from the columns whenever a ring changes (cheap: the grid tables are
  rebuilt on every flush anyway), so **M2b (toroidal addressing) is not needed**.
- **Columns**, on a pool: `sampleGrid` over 66 x 66 voxel columns (a border for the neighbours), each
  filled from its surface down to 4 voxels below its lowest neighbour, one 64^3 chunk per vertical cell
  touched, brick map -> tree -> tile table on the worker. Jobs are ordered by distance from the ring's
  centre (every ring together, so fast flight cannot starve the horizon), the hole last; a column the
  ring leaves before it is generated is cancelled.
- **Altitude**: a ring narrower than the camera's height over the ground is dropped; the next ring shows
  its hole instead.
- **Materials**: one per voxel. A spatial noise decides up close (patches, not a checkerboard), a per-voxel
  hash from 2 m voxels out (dither that averages to the mix). Both, and the crowns of open woodland, are
  thresholds against roughly uniform noise, so a weight takes the same share of voxels in every ring: an
  fBm threshold took 16% for a 25% cover and drew a line where two rings met.

| N (voxels, centre to edge) | 512 (default) | 1024 |
|---|---|---|
| Columns per ring | 16 x 16 | 32 x 32 |
| Reach of ring 10 | 131 km | 262 km |
| Chunks, voxel memory | 3,153, 76 MB | 12,709, 303 MB |
| Everything generated (16 threads, `--bench`) | 7.4 s | 30.0 s |

- A column: ~33 ms sampling (66^2 samples), ~8 ms filling and building tables, the same in every ring.
- Frame, settled (basic renderer, ~1 MP window): 15-17 ms. While streaming: 18-23 ms. 1440p would be
  about 3.5x that: M3 has to render at half resolution (as `fast` does) to meet G4.
- **Flight at 300 m/s, 400 m over the ground: streaming keeps up** (a few columns pending at times, in
  rings 1-3).
- Seams: none from geometry or colour statistics. A faint straight line remains where two rings meet on
  flat ground (voxel steps of two sizes side by side); M3's light may hide it.

## M3 as built (2026-11-21)

`examples/55-mountains/renderer/` (render.json, resources.json, shaders/, include/mountains_sky.sc), forked
from the gallery's `fast`; `MOUNTAINS_RENDERER=basic` keeps the engine's basic renderer.

- **Atmosphere** (`mountains_sky.sc`): two exponential layers (air 8 km, haze 1.2 km), optical depth in
  closed form along each ray, single scattering with the sun's light constant along the ray plus an
  isotropic share for multiple scattering. It is the sky, the ambient the SSAO gathers (sunlit ground
  below the horizon), and the aerial perspective on every surface (in `combine_blur`). The Jemez reads as a
  low blue band 60 km off.
- **Sun colour** on the CPU: Kasten-Young airmass through air, haze and ozone (Chappuis). Two of them: at
  the camera (lights surfaces) and 6 km higher (lights the air). With one, a low sun's orange times the
  air's blue made an olive-green sky; with the air's own (less reddened) sun and ozone it stays blue, and
  the range glows red-pink at a 4-degree sun.
- **Voxel-sized** shadow-ray offset and SSAO radius (4 voxels): the voxels run from 0.25 m to 256 m.
- **A depth pre-pass** (`depth_pre`, an eighth of the window each way): each G-buffer ray starts a little
  short of the nearest of the nine pre-pass rays around it. No artefacts seen.
- **Look**: a grade in `display` (contrast 1.08, saturation 1.2, a touch cool), exposure 1.7, albedos
  darkened to 0.70-0.95 of the map colours, soils pinker (adobe). `MOUNTAINS_AIR`, `MOUNTAINS_EXPOSURE`,
  `MOUNTAINS_SUN` for tuning; `[ ]` and `, .` move the sun.
- **Cost** on this machine (Radeon 860M iGPU, ~900 x 1160 window, the Plaza view, settled): GPU 17-18 ms:
  pre-pass 1.1-1.7, G-buffer 10.6 (camera rays ~7.3, shadow rays ~3.3), SSAO 0.5, combine 0.6, reconstruct
  2.5, resolve 0.8. The pre-pass saved only ~1 ms: the cost is not the empty rings in front but rays
  skimming the ground near where they hit. A mostly-sky view: 5 ms. `MOUNTAINS_DEBUG=noshadow,noskip`
  measures the parts; a capture prints GPU time by pass.
- Fixed on the way: `projv_add_example`'s staging target could copy a renderer folder before its shaders
  compiled (CMake 3.26+), so a fresh build ran with no `.bin`; it now waits for the shader target.
- Open: the horizon toward a low sun goes a little yellow (single scattering, no real multiple
  scattering); the renderer's speed on a discrete GPU.

## M5, first pass (2026-11-21)

Ahead of M4, because the dotted juniper hills are what reads as Santa Fe. `include/mountains/trees.hpp`.

- **Procedural trees, not assets yet** (D10 named the tree brush's assets for the nearest rings; shapes
  first, to judge density and scale): round bushy junipers and pinyons (2.5-6 m), ponderosas (bare orange
  trunk, oval crown, 14-24 m), firs and spruces (tiered cones, snow-dusted above the snowline), bare
  aspens. Stamped as voxels in rings 0-3 (voxels 2 m and finer).
- **Placement** is a jittered 6 m grid by world position, a cell's tree kept with the probability that
  makes the crowns cover what the canopy cover says; species from the elevation band (and aspect) at the
  trunk. Trunks stand on a 1 m grid of the ground aligned to the world, so neighbouring columns and rings
  agree on every tree.
- **Past ring 3**, a canopy voxel stands one voxel up off the ground where its trees are at least 0.6 of a
  voxel tall, so woodland keeps a silhouette and shadow (PJ to the 4 m ring, forest to the 16 m ring) and
  the hand-off from drawn trees shows no edge. As a flat tint it did: drawn trees hide the ground at a
  grazing angle, a tint does not.
- PJ cover 18% (was 25%). Generation: 7.5 s for everything (fill ~26-51 ms a column in rings 0-3).
- Open: crowns are one green each; ponderosas read as lollipops; snags, shrubs (chamisa) and the
  canyonlands juniper and pinyon assets for the nearest ring.

## M5, second pass (2026-11-21): less clean, more colour

The user, on the first pass: "the shape is incredibly good"; the trees too clean and uniform, the colours
boring.

- **Crowns are lobe clusters eroded by 3D value noise** (ragged edges, gaps), each tree leaning a little,
  sized from a skewed spread (many small, a few big), with two foliage colours in patches, its own shade
  shift, darker inside and lighter at the top and rim. Kinds: shaggy junipers with twisted trunks, denser
  pinyons, silver snags (3.5%), ponderosas with crowns in clumps up a cinnamon trunk (old ones
  flat-topped), firs and spruces in drooping tiers with a spire, aspens with white trunks.
- **Understory** on its own 3 m grid: Gambel oak thickets (drawn from 1 m voxels) and chamisa with faded
  yellow tops (from 0.5 m); a tint beyond.
- **October by default** (`MOUNTAINS_SEASON=november` for the bare aspens and more snow): gold aspen groves
  kilometres across in the mixed conifer and spruce-fir, rust oak on the mid slopes (south-facing more),
  gold grama, a first dusting of snow on the peaks. Ground: red earth patches, pale sand in the washes,
  pink granite with orange lichen. 26 classes, 8 shades each (208 slots).

## D2 revised (2026-11-21): voxels near, a smooth heightfield far

The user, on the renderer: distant voxels should not be visible; screen-space normal smoothing over the
far rings looked low-quality at distance (on a gentle slope one voxel step spans more than the kernel, so
contour bands stay). Agreed instead: **voxels to ~2 km, a smooth surface beyond.**

| # | Decision | Why |
|---|---|---|
| D14 | **Voxel rings 0-4** (0.25-4 m voxels, ~2 km); **far rings 5-10 are heightfields**, same clipmap and columns, no voxels | Voxels where they read as voxels and carry the detail (trees to 1 km, canopy bumps to 2 km); beyond, a voxel is a few pixels and only its edges show |
| D15 | **Raymarched, not rasterized**: each far column writes a 64 x 64 tile of height (R32F) and colour (RGBA8) into a per-ring atlas, by column modulo the ring's span (toroidal), uploaded as a sub-rectangle; a small state texture says which column each slot holds. The G-buffer pass marches the heightfield (bilinear heights, normals from them) after the voxel march misses, from where the ray leaves the voxel rings' box | The engine's renderers are fullscreen passes: a mesh pass with a depth buffer is an engine change. A heightfield march fits the pass as it is, reads as a smooth mesh, and shadows and aerial perspective come free |
| D16 | Far colour is the surface mix blended, not dithered; forests raise the far surface by their canopy | No speckle; forests keep a silhouette and a shadow to the horizon |
| D17 | Far hits are written as non-voxel (gFace.w = 0), so the reconstruction upsamples them bilinearly | A smooth surface has no voxel faces to rebuild |

Milestone **M7**: the far field; the voxel rings cut to five; the hand-off measured and judged. The
screen-space normal smoothing (normal_smooth) stays for the outer voxel rings near the hand-off only.

## M7 as built (2026-11-21): the far field

- `include/mountains/columns.hpp` `generateFarColumn`, `renderer/include/mountains_far.sc`, the far rings in
  `main.cpp`. Five voxel rings (`MOUNTAINS_VOXEL_RINGS`), six heightfield rings: 1,620 chunks and 34 MB of
  voxels (was 3,288, 78 MB); far columns cost ~35 ms each, no tile tables.
- The atlases: heights R32F and colours RGBA8, 3072 x 2048 (six 1024^2 ring squares), and a 96 x 16 state
  texture; a column's tile goes up as a sub-rectangle when it installs.
- The march: bilinear heights, the ring chosen by distance from the camera, one state lookup per column a
  sample's four texels fall in, steps of `max(0.7 texel, 0.0012 t, 0.45 * height above)`, the crossing
  found by halving. **The depth pre-pass marches it too**, so a far ray starts just short of its hit.
  Shadows: the voxels, then the far field (40 km).
- **Detail finer than the heightfield** (the user: distant mountains too smooth): octaves of gradient noise
  in the normal, from just under a texel to a pixel and a half, stretched three times down the slope (reads
  as gullies and ribs), strongest on steep ground, with a matching tone in the albedo.
- Far colour takes 1.7x the canopy cover, darkened, for how a forest hides its ground seen from the side.
- The screen-space normal and albedo smoothing (`normal_smooth`) now only touches voxels under 3.5 render
  texels (the user: it started too close); far hits pass through it.
- Cost on the Radeon 860M, ~1 MP: GPU ~21 ms (G-buffer 13-14, pre-pass 1.5). Before the pre-pass marched the
  far field: 19 ms G-buffer.
- `--selftest` now flies out and back and requires the same chunks and blobs live at the end as at the
  start (a leak test that does not depend on how wooded the ground is).
- Open: the hand-off at ~2 km shows (voxel woodland against smooth ground); the detail's strength; speed.

## M6, so far (2026-11-21)

- `examples/55-mountains/README.md`; a row in `examples/README.md`; a run line in AGENTS.md.
- `./mountains --selftest` (headless, ~45 s): generates every ring around the Plaza, then flies 5 km east
  in 250 m steps; each step checks that every column produced ground, every grid cell names a live chunk
  of its own ring, and rows and blobs stay bounded (rows track the live chunks: freed rows are reused).
- Camera presets `plaza`, `atalaya`, `baldy`, `aerial` exist but are rough: the user is to place the
  showcase views (AGENTS.md: camera positions are asked for, not searched for).

## Open questions

1. N per ring: 512 (cheaper, ~2.7 px voxels at range) or 1024 (~1.3 px, ~4x memory). Measured in M2.
2. Can 11 grids be marched in the budget? Each ray walks every ring it crosses until it hits. If not, an
   engine change makes the march go from ring to ring (the next ring takes over where the ray leaves the
   hole) instead of marching every grid. This would be its own milestone.
3. Float precision far from the origin: at 100 km a float is ~8 mm, fine for 0.25 m voxels. Walking
   hundreds of km would need a floating origin. Not now.
4. Physics and walking: a player camera with ground collision from the height function (no Jolt) is
   enough for M6. Physics on the near ring is out of scope.
5. Should the far field ever be a heightfield after all? The user chose voxels all the way. Revisit only
   if M2/M3 can't meet G4.

## Sources

1. https://en.wikipedia.org/wiki/Santa_Fe,_New_Mexico
2. https://en.wikipedia.org/wiki/Atalaya_Trail
3. https://geoinfo.nmt.edu/tour/state/hyde_memorial_park/home.html
4. https://en.wikipedia.org/wiki/Ski_Santa_Fe
5. https://en.wikipedia.org/wiki/Santa_Fe_Baldy
6. https://theoutbound.com/new-mexico/hiking/hike-santa-fe-baldy
7. https://www.nps.gov/places/redondo-peak-resurgent-dome.htm
8. https://en.wikipedia.org/wiki/Sandia_Mountains
9. https://en.wikipedia.org/wiki/Ortiz_Mountains
10. https://en.wikipedia.org/wiki/Caja_del_Rio
11. https://www.nps.gov/articles/000/new-mexico-la-bajada-mesa.htm
12. https://ngmdb.usgs.gov/Geolex/UnitRefs/SantaFeRefs_9963.html
13. https://www.bivy.com/adventures/us/New%20Mexico/nambe-lake-trail-402001
14. https://pubs.usgs.gov/publication/70273780/full
15. https://en.wikipedia.org/wiki/Tesuque_Formation
16. https://ngmdb.usgs.gov/Geolex/UnitRefs/TesuqueRefs_10831.html
17. https://geoinfo.nmt.edu/publications/periodicals/nmg/24/n3/nmg_v24_n3_p75.pdf
18. https://www1.usgs.gov/csas/nvcs/unitDetails/899634
19. https://edit.jornada.nmsu.edu/catalogs/esd/035X/F035XG006NM
20. https://santafenm.gov/document_center/document/780
21. https://fs.usda.gov/r03/santafe/wilderness
22. https://webarchive.library.unt.edu/eot2008/20090114061517/http:/www.fs.fed.us/r3/sfe/wildlife/SFNF_habitats&wildlife.pdf
23. https://home.nps.gov/grsa/learn/nature/naturalfeaturesandecosystems.htm
25. https://www.fs.usda.gov/air/technical/class_1/wilds.php?recordID=87
26. https://en.wikipedia.org/wiki/Sangre_de_Cristo_Mountains

Technique:

- IQ, derivative fBm: https://iquilezles.org/articles/morenoise
- runevision, erosion filter and Phacelle noise: https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html
- Gaillard et al. 2019, Dendry: https://www.cs.purdue.edu/cgvlab/www/resources/papers/Gaillard-2019-Dendry_A_Procedural_Model_for_Dendritic_Patterns.pdf
- Cordonnier et al. 2016, stream-power terrain: https://www.cs.purdue.edu/homes/bbenes/papers/Cordonier16CGF.pdf
- Losasso & Hoppe 2004, geometry clipmaps: https://hhoppe.com/proj/geomclipmap/
- Hillaire 2020, scalable sky and atmosphere: https://diglib.eg.org/handle/10.1111/cgf14050

# Mountains: the Sangre de Cristo at Santa Fe

Status: **approved**, 2026-11-21. **M1 built** (awaiting the user's look); M2 in progress. A new example, `examples/55-mountains` (target
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
| G4 | Fast | >= 60 fps at 1440p on this machine standing still; flying at 300 m/s, the horizon keeps up |
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
- Cost: ~25 us a sample at full detail (16 threads: a 1600^2 map in 4 s). Too slow for M2 as is: the macro
  is evaluated five times per sample (for its gradient) and every octave per column. M2 splits the sampler
  in two: the macro and the long-wavelength octaves on a coarse lattice per chunk, interpolated (height
  and gradient), and the short octaves per column.
- Open: random provinces outside the layout read as isolated stamps (capsule ranges, round mesas). Basin
  and range chains, en-echelon ranges and dissected basins are later work; the Santa Fe layout is the
  priority.

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

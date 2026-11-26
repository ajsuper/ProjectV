# Santa Fe: a quality reference

Status: **in progress**, 2026-11-23: S0-S2 built, S3 first pass, S4 optimized mode, S5 game scale + placements + character, S6 upscaler + boulders, S7 rock as boulders + generation speed, S8 relief everywhere + scale 1.5, S9 gentler cirques + living rock, S10 blocky distance, S11 stable reconstruction + shadow blur + clouds as their own pass, S12 soft quarter-res glow + blocky style, S13 box clouds rebuilt like voxels + tunable glow, S14 cloud-shadow map, S15 normals group + separate blocky toggles, S16 atmosphere: leaf SSS, motes, light shafts, S17 leaf light + traced AO, S18 cheaper shadows and AO, leaf glow only through one layer, S19 meadows, flowers, snow on trees, S20 glints, sticks, clumps, S21 glints as a grid of mirrors, sheen, screen-space reflections, S22 a clean image: no history on surfaces, upscale per layer, S23 the sky in the render: clouds that darken, fog and cloud settings, weather, S24 rendering polish: bloom, clouds, tone, S25 light and colour calibrated, S26 faster (14.3 -> 12.3 ms), S27 trees that match at every distance (coarse levels by area, far crowns from the same trees), the sky not traced. A new example, `examples/56-santa-fe` (target `santa_fe`), forked from
`55-mountains` (its working tree as of 2026-11-23, M4a props included). `55-mountains` is left as it is.

The user, on `55-mountains`: it feels flat, uninteresting and undetailed. Make it far better looking, with
the focus on **rendering a high-quality image**, so we can see what a future optimised version needs. Wanted:
higher quality small-scale models, more detail on the ground, more variety, clouds and more volumetrics,
and distant terrain that reads as real terrain -- the bumps of trees and patches of dirt visible from many
km away. Trees a little bigger than real (more interesting). Smoothness for rocks.

Speed is not a goal here. A frame may take seconds; a still converges over many frames. What each feature
costs is written down, for the optimised version.

## Decisions

| # | Decision | Why |
|---|---|---|
| R1 | **A progressive reference renderer**: full resolution, one path per pixel per frame, accumulated while the camera is still (reset when it moves). No half resolution, no reconstruction, no screen-space AO | Noise converges to the right answer; nothing is approximated that a later fast version would have to be compared against |
| R2 | **Per frame, per pixel**: a jittered camera ray (antialiasing); a sun shadow ray toward a random point of the sun's disc (soft shadows); one cosine-weighted diffuse ray traced through the voxels and the far field, lit at its hit by the sun through a second shadow ray (one bounce of GI) or by the sky on a miss | Real sky occlusion and bounce light: the warm light under trees, in canyons and off sunlit slopes is most of what reads as "not flat" |
| R3 | **Every ray through one march call site** (a loop over the path's segments) | The repo's rule: a second inlined march doubles the cost of every mode |
| R4 | **Volumetric clouds**: a cumulus layer (bases ~4.3 km, tops ~7 km), denser over the high ground, density from procedural 3D noise; raymarched on camera rays with a light march toward the sun (Beer-powder, two-lobe phase, multiple-scattering octaves); **cloud shadows on the ground** from the same density along the sun ray | Santa Fe's afternoons are cumulus over the Sangres; cloud shadows dappling the range are what give a distant slope variety and depth |
| R5 | **Low haze in the valleys**: a height fog in the Rio Grande basin, lit by the sun with the terrain's shadow on it (a few shadowed samples per camera ray) | Volumetrics: layers of distance and light shafts |
| R6 | **Smooth surfaces from occupancy**: at a hit on a material flagged smooth (rock, boulders, snow), the normal is the gradient of the voxel occupancy in a 5^3 neighbourhood (probed in the hit chunk) rather than the face's | Rocks read as rounded stone, not stacks of cubes, at no cost in memory. Flag: the palette entry's `metallic` byte (this renderer's own use; nothing else draws this scene) |
| R7 | **Far trees are geometry**: the far march adds canopy domes on a jittered grid (the near trees' grid), kept by the canopy cover carried in the colour atlas's alpha, each tinted from a canopy colour atlas; they are marched and they cast shadows. Fade to their mean height where a crown is under a pixel | The dotted pinyon-juniper hills and the textured forest from 10 km: bumps with sunlit tops and shadowed sides, not a speckle in the albedo |
| R8 | **Far ground in patches**: ground colour (dirt, grass, rock) apart from canopy colour, plus height detail in the march (not only the normal) within ~15 km | Patches of dirt between the trees, and silhouettes with relief |
| G1 | **Trees about 1.4x their real size** | The user: not realistic, but more interesting |
| G3 | **One voxel scale**: ring 0 is 0.1 m (rings 0.1 .. 3.2 m, six of them; the far field from 6.4 m), and trees and props are grown at ~0.1 m too (the tallest trees 0.15 m; props 16^3, logs 64^3) | The user: 0.25 m ring 0 felt wrong at this scale, and finer assets on coarser ground read as two scales |
| G4 | **Trees from the scene editor's tree brush** (`60-scene-editor/brushes/trees.lua`), grown at startup through its brush host (`brush.cpp`), as `terrain_asset_baker` grows 50's assets; cached in `tree_cache/` | The user: the editor's generator is more mature and higher quality than trees.hpp's |
| G5 | **Game scale 2x** (`surface.hpp kGameScale`): the player and everything living stand twice their size against the landform and the 0.1 m voxels, so a voxel is 0.05 m to the player; trees stay 1.4x real to the player | The user: the world shrunk around the player at the same resolution -- a game-friendly scale; trees and props get twice the voxels for nothing |
| G6 | **Placements, axis-aligned** (`placement.hpp`): one shape library at the lattice's voxel (tall trees a whole multiple), quarter turns only. Near rings (0.1, 0.2 m) keep them as loose objects on the lattice; coarser rings bake the same placements into their voxels | The user: everything aligned with the terrain; near things stay objects for game mechanics, far ones baked to draw fast. One library is why a shrub can no longer be finer than its neighbours |
| G2 | **More on the ground**: grass tufts, more cobbles and boulders, litter; more variation in where things grow | "More detail on the ground, more variety" |

## Milestones

| # | Milestone | Done when |
|---|---|---|
| S0 | The fork builds and captures as 55 did | Baseline captures (Plaza, aerial) |
| S1 | The reference renderer (R1-R3, R6), clouds and cloud shadows (R4), valley haze (R5) | Captures compared with the baseline |
| S2 | The far field as terrain (R7, R8) | An aerial and a Plaza capture show tree bumps and dirt patches kilometres out |
| S3 | Generation: G1, G2, finer near models | Walk captures |
| S4 | What the optimised version needs: a table of each feature's cost and the cheapest version of it that looked the same | Written here |

## As built

- **S0** (2026-11-23): `examples/56-santa-fe` from 55's working tree; `santa_fe`, `santa_fe_lab`. Note: it
  relies on 55's uncommitted engine changes (`pjvQueryLooseReach`, `PJV_SCENE_GRIDS_FIRST`, the loose-list
  signature) being in the tree.
- **S1** (2026-11-23): `renderer/` is four passes: `depth_pre` (1/8, voxels + far field with crowns + props),
  `trace` (full resolution, one path a pixel a frame: camera, soft sun shadow, cosine diffuse ray, its sun
  shadow, one haze-shaft shadow ray, all through one march site), `accumulate` (running mean, reset on any
  change), `display` (exposure, grade, glow, vignette). Clouds: `include/santa_fe_clouds.sc`, a 128^3 tileable
  Perlin-Worley/Worley noise baked on the CPU (1.1 s) into a 2080 x 1040 atlas; coverage from a weather map
  plus an orographic boost from the far field's height; Earth curvature so the deck sinks to the horizon.
  Smooth rock (R6): the palette's `metallic` byte; coarse ground (from ~0.15 m voxels) is smoothed too, and
  from 0.6 m shaded by the far field's ground normal (the steps of a slope lit as stripes otherwise).
- **The dev panel** (F1, "Santa Fe"): every feature a switch (`SF_*` bits in `mountains_far.sc`, `sfFlags`),
  sun, air, clouds, haze, far field and display knobs, print camera, save capture, max samples.
  `SANTA_FE_FLAGS` / `SANTA_FE_OFF` (bit masks) and `SANTA_FE_SAMPLES` (capture after N samples) for captures.
- **S2**: the far atlases carry the ground without trees (colour, cover in alpha) and a canopy atlas (colour,
  height, conifer bit); `mtnCanopyAt` stands crowns on a jittered grid, marched and shadowed, faded to their
  mean under ~0.4 px. Ground patches in the far colour.
- **S3, first pass**: trees 1.4x (G1); ring 0 0.1 m (G3); brush trees in rings of 0.4 m and finer, as loose
  props turned at random and sized to the stand within 0.75-1.3x; grass tufts (0.6 m grid), coarser props.

### Cost (Radeon 860M iGPU, ~900 x 1160, settled)

| View | trace | frame |
|---|---|---|
| Plaza (near trees, clouds) | 490 ms | 515 ms |
| 400 m up, the range | 540 ms | 570 ms |

### S4: what each feature costs, and the optimized mode (2026-11-23)

The user, on the reference: what sold scale most was shadows, the far trees as geometry, clouds, distance fog,
then the close rocks and bushes; the advanced lighting mattered less. `SANTA_FE_BENCH=1` measures it: once
settled, each feature off in turn, GPU ms in one window (Radeon 860M, ~900 x 1160, 400 m up looking at the range):

| Off (from everything on, 586 ms) | Saves |
|---|---|
| traced sky light + bounce | 325 |
| far trees as geometry | 164 |
| sun shadows | 105 |
| clouds | 75 |
| far detail normals | 50 |
| haze light shafts | 44 |
| smoothing, cloud shadows | 30 each |
| the rest | under 5 |

The primary march alone is 56 ms. **Optimized mode** (the default; `SANTA_FE_MODE=reference` for everything):
no traced sky light or bounce, hard shadows, no shafts; clouds 28 steps with 4 light steps; smoothing radius 1;
far shadows to 20 km; 60% of the far detail octaves. Plus, for both modes: the far march evaluates crowns only
within reach of the canopy, steps short among crowns only where they are drawn as crowns (not where they are
their mean height), and tests the nearest 2 x 2 crowns, not 3 x 3; cloud coverage reads one coarse far ring.

| View | Reference | Optimized |
|---|---|---|
| Walking, near trees | ~490 ms | 63 ms |
| 400 m up, the range | ~540 ms | 96 ms |

Next for speed: render the trace at a fraction of the window and upsample (the primary march is most of what
is left), clouds at quarter resolution with temporal reprojection.

Also from this round: grade defaults 1.2 contrast, 1.25 saturation, a vibrance term; ground mostly grass
(55's half-and-half bare earth reads as a sand pit); a voxel turf of grass blades on grassy ground in the two
finest rings; more tufts, chamisa and snakeweed.

### Found on the way

- **A prop's resolution must be a power of 4** (16, 64, 256): `buildTileTable` at 32 or 128 returns an empty
  table, so the prop never draws. 55's M4a library (uncommitted) has boulders, cholla and yucca at 32: none of
  them has ever drawn. Fixed here (and `toTiles` asserts it); 55 still has it.

### Open

- Thin dark contour lines on coarse ground seen from a few hundred metres up (not cured by starting shadow
  rays clear of the next step: cause still open).
- The far field's crowns are denser and darker than the voxel rings' trees at the ~2 km hand-off.
- Far rock faces are flat orange: no rock detail in the far field yet.
- A ring-2 tree near ring 1's edge can be drawn twice (the prop, and ring 3's stamp of its crown).
- Speed: ~0.5 s a frame here; S4 is to measure each feature and find the cheap version of each.

- **S5** (2026-11-23): G5, G6; brush trees at two heights a design (46 shapes), no per-instance scaling (their
  voxels stay on the lattice); six shades a material so the brush's 63 colours (and 13 of ours) fit in every
  ring's palette, as baked trees need. **The character** (`main.cpp Character`): a 0.6 x 1.8 m box (the
  player's), collision with ring 0's voxels (`utils::queryVoxelMaterial`) and the solid loose objects (trees,
  stones, logs, cactus; grass and shrubs are walked through), gravity, jump, run, 0.3 m step-up. Starts walking
  (G flies; a camera placed in the air starts flying). Generation is 17.5 s for everything (was ~10: baking).

- **S6** (2026-11-23): **render scale and an upscaler**, after restir-pt: the trace pass and its G-buffer (colour,
  position, face normal, voxel face and edge, chunk rotation, motion) at a share of the window (the panel's
  "render scale", 0.5 in optimized mode, set live through the engine's relative texture scales); restir-pt's
  **voxel reconstruction** rebuilds the window voxel by voxel; a **resolve** accumulates when still and
  reprojects with a neighbourhood clamp when moving (no reset on a camera move or a streamed column; a knob
  resets it). Optimized walk view: 89 ms -> 35 ms (0.5), 24 ms (0.35), the image near-identical.
  **Boulder formations** from the editor's Boulders brush (`boulders.lua`): 20 (granite tors and piles,
  clusters, splits, gneiss trains, basalt columns and talus, sandstone slabs, erratics), the largest at 0.2-0.3 m
  voxels (the brush caps a placement at 110,000 voxels), placed by the rock (`placement.hpp` Formation).
  **Rock faces built of boulders** (the user): exposed rock is covered with big boulders packed close and sunk
  into the slope. Only the brushes' materials in use are interned (109 of 147; 254 slots with the terrain's
  5 shades). The smoothing probe now drops pairs that leave the chunk (it drew lines on every chunk seam).
- Open: snow terrace edges glint (the gloss on each step's riser); far-field rock faces are still smooth (the
  boulders reach to the voxel rings' edge, ~3 km).

- **S7** (2026-11-23): the user: the boulders did not blend with the flat exposed rock, and many floated.
  **Rock as boulders** (`surface.hpp rockBlocks`): where the ground is rock it is raised into rounded, jointed
  blocks -- a Worley field of 8 m blocks and 2.8 m ones over them (world units), each block its own height and
  tone, dark joints between -- prefiltered by footprint, in every voxel ring and (as the mean) the far field.
  **Nothing floats**: stones, logs, formations and the rock-face boulders rest on the lowest ground under their
  turned footprint (nine points, the rock blocks included). **Generation**: 19 s -> 9.4 s for every ring --
  shape pyramids so a coarse ring stamps one sample a voxel (ring 2-4 columns 210-227 ms -> 50-80), the
  placements' ground on a 2 m grid, kinds too small for a ring skipped whole, the fine rings sampling the
  landform at the next ring's lattice and interpolating (26 -> 10 ms a column). Flying: a ring that would cross
  its half-width in under two seconds is dropped while the camera moves that fast, so the coarse ones keep up;
  the install budget is 10 ms a frame.

- **S8** (2026-11-23): the user: the terrain felt too fine, the trees too big to the player, and the ground
  too smooth beside the new rock faces. **Game scale 1.5** (a voxel 0.067 m to the player), **trees 1.1x**
  real. **Relief everywhere** (`rockBlocks`): every ground is the block field -- bare where it is rock, buried
  under soil as low hummocks elsewhere, the share of bare blocks rising with the rock (boulders emerging from
  the ground, no line), clods a hand high over all. Bare blocks are rock whatever the mix picked; buried ones
  are ground. Trees stand on the relief. Generation 8.9 s.

- **S9** (2026-11-23): the user: cliffs high up far too steep for these mountains; the rock faces good but too
  uniform, with no moss or plants in the cracks. **Cirque headwalls ~45 degrees** (the bowl `650 d^2.2`, was
  `1100 d^2.6`: 60-70 degrees where it met the crest). **Two block styles by region** (massive and finely
  fractured, ~90 m regions, the two meeting where both show) and regional bands of tone; lichen in patches
  (was a uniform 7%). **Moss** in the joints, more on the shaded side (pinyon's green or dry grama: the
  palette is full), a cushion standing in the joint in the fine rings. **Plants in the joints**: grass,
  snakeweed, chamisa, now and then a small juniper or pinyon, only where the anchor falls in a crack between
  bare blocks.

- **S10** (2026-11-23): **blocky distance** (a style toggle, off by default: the panel's Far field, or flag
  bit 18): the far field as flat-topped blocks of `sfFar.x` far texels (1, 2, 4, 8; 4: 25 m at 3 km, 800 m
  on the horizon), marched exactly block by block, restarting on each ring's blocks; whole columns skipped
  where the ray stays over the column's highest point (`farMax`, from the CPU). Blocks report their faces
  to the reconstruction (crisp edges); a block's colour is its ground and trees by cover. Generation follows
  the style: a far column samples and colours one point a block (64 -> 16 a side at 4) and the far rings
  regenerate when the style or the block changes. Measured (one batch, the range view, optimized, scale 0.5):
  trace 16.8 ms blocky against 39.4 ms smooth; far columns 42 -> 3.8 ms; everything generated 10.4 -> 6.5 s.
  Second pass (the user: the blocks far too light, not coloured by the trees): a block's colour is its trees by
  how much of them is *seen* (`1 - exp(-8 cover)`: crowns hide and shade the ground between them; matched by eye
  to the smooth far field), the ground darkened under them. A block stands at the lowest of its centre and four
  points toward its corners; a hit within a block and a half of where the march (re)started is a top, not a
  wall. And the dark line along a far ring's square: the march picked a ring by distance alone, and where that
  ring did not hold the column it slid past the ground; it now falls back outward as the smooth march does.

- **S11** (2026-11-23):
  - **The upscaler** (the user: temporal upscaling visible on flat faces). The reconstruction claims nearly every
    pixel (its debug view); the cause was texels of one face disagreeing -- chiefly the cloud shadow's per-pixel,
    per-frame random step offset -- so the face's rebuilt colour moved with the jitter and the resolve was seen
    averaging it. The offset is now fixed in the world: frame 1 against frame 32, RMSE 5.9% -> 0.8%. Per-face
    shading (shadow, cloud shadow, ground normal at each face's centre) was tried and kept as a toggle, off:
    it only took that to 0.65%, and costs shadows that fall across a face.
  - **Shadow blur** (optimized mode; `shadow_soft.frag`): the sun's light is written apart with its visibility and
    the blocker's distance; a screen-space PCSS blur (penumbra = blocker distance x softness, same surface only)
    puts it back. Contact shadows sharp, long ones soft, no per-frame noise. ~0.4 ms.
  - **Clouds**: taller and grander (a second weather map for how tall a cloud grows, to congestus near 10 km;
    domain-warped two-scale billows; denser cores; less ambient: grey bases), denser and harder-edged; to the
    horizon (300 km, steps lengthening with distance); a cirrus sheet at 10.5 km. **Their own pass**
    (`clouds.frag`): a quarter of the trace's texels, marched to the farthest surface in the footprint, steps
    offset per frame, blended with its history reprojected at the cloud's distance (from the texel's centre, so
    a still view reads it exactly); rejected where the surface behind changed depth or a setting changed;
    composited (Catmull-Rom up, behind-surface depth test) in `shadow_soft`. Sky view: the clouds 6.8 ms, from
    ~15 inside the trace, and smooth where they were grainy.
- Reuse still on the table: a top-down cloud-shadow map once a frame (the trace's 6 density samples a texel);
  the glow at a quarter of the window (~2 ms); sky rays walking every voxel ring before they leave.

- **S12** (2026-11-23): **the glow** in its own pass at a quarter of the window (`glow.frag`: a soft knee over the
  scene's whites, 9 wide taps; added at 0.12, was 0.35 and overexposing). 1.3 ms against ~2.8 in the display
  pass. **Blocky style** (the toggle was "blocky distance"): the far field's blocks and **box clouds** -- body
  boxes of 520 m read from the billows at each box's centre, detail boxes of 170 m from the eroded edge at
  theirs -- marched exactly box by box (a two-level 3D DDA: a solid body box one segment, an empty one skipped,
  a partly cloudy one walked through its 3 x 3 x 3 detail boxes), each box flat-lit. Their shadows on the ground
  follow the boxes. (Fixed-length steps through boxes this size dithered their edges.) Sky rays leaving early
  is deferred: the clouds are to come down over the mountains later.

- **S13** (2026-11-23): **box clouds with the voxels' edges** (blocky style): each cloud texel records the size of
  the box its ray met first (`cloudDepth.z`); `shadow_soft` tests each texel's own ray against the boxes its
  3 x 3 neighbours recorded, takes the nearest it enters (its light, and its face) and hands that face to the
  reconstruction in place of the trace's -- `shadow_soft` now writes the G-buffer the reconstruction reads -- so
  boxes get exact, antialiased edges at the window's resolution. Boxes under two texels are left to the smooth
  composite. Boxes are lit by face (tops bright, sides by the sun, bottoms dark). **The glow is tunable**:
  threshold, knee, spread and strength in the panel.

- **S14** (2026-11-23): the user: cloud shadows vanished once the clouds came down. Each texel estimated its cloud
  shadow from 6 density samples over at most 12 km of the sun's path: a lowered, so thicker, layer put them
  kilometres apart, stepping over clouds a few hundred metres across. Now **a cloud-shadow map**
  (`cloud_shadow.frag`, before the trace): 256 x 256 over 32 km round the camera (its corner snapped to its
  texels), each texel the transmittance of the sun's path up through the whole layer from its base, in 48 steps;
  a texel of the trace reads it with one lookup, its point carried along the sun to the base. Beyond the map,
  the old estimate. Correct at any cloud height, and cheaper a texel (the map pass ~7 ms on the iGPU: 256^2 x 48).
  The rest of what the user saw: the shape noise was read by altitude, so a lower base read another slice of
  it -- other clouds, their shadows kilometres elsewhere. It is read by height within the layer now: lowering
  the layer lowers the same clouds, and their shadows come with them (checked at 4300, 3200, 2600 m).

- **S15** (2026-11-23): **blocky far ground and box clouds are separate toggles** (Far field / Clouds). A second
  flag word (`sfFlags.y`, `SF2_*`: a float holds 24 bits exactly, and the first word was full). **Normals**, a
  panel group with a master "normal blend": smooth rock and snow (as before); smooth coarse ground (split out of
  it); smooth everything else (leaves, wood, props, by a share); and **rounded outside edges** -- after
  restir-pt's `normal_blur`, but analytic in the trace, where the shading is: within the bevel width of a voxel
  face's edge the shading normal leans toward that side, fading to nothing at the width, where the neighbouring
  voxel that way is empty (an outside edge; an inside corner stays crisp). Up to two probes a hit; no measurable
  cost. Width and strength tunable; on in both modes.

- **S16** (2026-11-23), atmosphere, all cheap enough for optimized mode and no extra rays:
  - **Leaf SSS** (trace): leaves wrap the light round their terminator; and light through them -- how much
    reaches a leaf is its sun shadow ray's own answer (all of it in the open, falling off as `exp(-blocker /
    depth)` with how far the ray got into foliage: an outer leaf glows, a crown's heart does not), seen most looking
    toward the sun through it and on its far side, yellower and more saturated. Far crowns too.
  - **Dust and pollen** (display, after the resolve so they move crisp): motes in a grid of world cells, one or
    none a cell, drifting and bobbing; the view ray walks the cells within ~25 m behind what the pixel sees; soft
    dots no smaller than a pixel (fainter if they would be), lit by the sun with a forward lobe.
  - **Light shafts** (glow, a quarter of the window): from each pixel toward the sun on screen, the open sky
    (the trace's depth) along the way, decaying -- ridges, crowns and cloud bases cut it into rays.
  - ~1.5 ms together on the iGPU. Panel: Atmosphere. Also: duplicate widget labels fixed (ImGui IDs).

- **S17** (2026-11-23): the user, on S16's SSS: rather a leaf highlight, softer grass shadows, an outline on trees
  seen from their shaded side; and a real AO (under trees the shade was flat). **Leaf light**: a sheen where a
  leaf faces between the sun and the eye (with the sun, so under its shadow and blur), wrap, a faint glow
  through (0.2, was 0.45). **Soft shadows from thin things**: a sun shadow ray stopped within 2.5 m by grass
  (Grama and Tundra now carry a little transmission) lets 55% through; by leaves, ~18% fading with depth --
  grass shadows soften and light dapples through crowns; a far blocker (a hill) stays dark. **Leaf outline**
  (`shadow_soft`): a leaf texel (flagged in gSun.a + 2) at its crown's edge -- something much farther or the sky
  within two rings of texels -- catches the sun behind it, graded, warm. **Traced AO** (optimized mode; the
  trace's sixth segment): one cosine-weighted ray a texel, 6 m (the player's) reach, 32 steps, coarse with
  distance, voxels and props; denoised edge-aware in `shadow_soft` (12 taps, normal and plane), applied to the
  sky light's share of the colour (the trace's targets are full: share and AO ride in gMotion.zw), then the
  resolve's history. +3.5 ms of trace on the iGPU.

- **S18** (2026-11-23): the user, on S17: the outline does not look good (removed); leaves in shadow glow, so it
  reads as a glow, not light through leaves; the AO is right but too slow (a lower-resolution pass, screen-space
  AO for the fine detail); cloud shadows are a large share of the frame; shadow rays need not be that good.
  - **Leaf glow through one layer**: the glow only where the sun's path is clear (vis > 0.9) or the shadow ray
    stopped at a single voxel of foliage right at the leaf (within 1.5 voxels, transmission >= 0.25: 55%); no
    constant term. A leaf in another thing's shadow -- the heart of a crown, a crown under a ridge -- does not glow.
    Outline gone (shader, flag bit 256, knobs).
  - **AO, two parts**: the traced ray left the trace for its own pass, `ao_trace.frag`, at half the trace's
    resolution (a quarter of the rays), out to 300 m, accumulated over 16 frames (reprojected by the trace's
    motion, dropped where the depth disagrees). `shadow_soft` upsamples it along the surface (its 12-tap normal
    and plane blur, now reading the half-resolution answer) and multiplies in an 8-tap **screen-space AO** at the
    trace's resolution (0.4 m of the player's, at most 24 texels): the creases the half-resolution texel blurs
    away. Forest floor: 3.5 ms of trace -> 1.7 ms. Panel: AO traced out to, AO screen-space detail, SSAO radius
    and strength.
  - **Cloud-shadow map, a sixteenth a frame**: the map pass reads its own last output (ping-pong) and re-marches
    one texel of each 4 x 4 block a frame in turn (all of them on the first frames and when the look changes);
    the rest carry over, read where the same ground was in last frame's map. 32 steps up the layer (was 48).
    7.1 ms -> 1.4 ms.
  - **Shadow rays, measured first**: a checkerboard of shadow rays (half traced, half filled from neighbours)
    saved nothing -- a wave runs 32-64 neighbouring pixels in lockstep, skipping alternate ones idles lanes. Nor
    did coarser voxel LOD (48 voxels), a cap on the voxel part (40-120 m) or on the props' reach. Split by part:
    the voxel part 2.8 ms on the forest floor, 0.2 ms from 400 m up; the **far field's part** (heightfield and
    crowns) 0.7 ms and **6 ms**. So the far part moved to the **clouds pass** (a quarter of the trace's texels;
    it already reads the G-buffer and every far texture): one march per cloud texel from its nearest surface,
    out in cloudDepth.w (floor(depth x 2) + visibility x 0.99); `shadow_soft` takes the four round each texel,
    bilinear and weighed by depth, on the sun's light and on the leaf glow's share of the colour (the trace's
    gMotion.w, freed by the AO) -- a crown under a ridge does not glow. 400 m vista: trace 39.6 -> 34.6 ms,
    clouds +1.6 ms; the image the same (ridge and far-crown shadows hold at that resolution). Kept: the voxel
    part's coarse march past 48 voxels and a 120 m voxel cap (the far field past it). Optimized mode; panel:
    far shadows at quarter resolution, shadow rays: voxels for, coarse past. `SANTA_FE_SHADOW_VOXELS`,
    `_SHADOW_LOD`, `_SHADOW_PROPS`, `_SHADOW_DEBUG` (1 voxels only, 2 far only), `_NO_SSAO` for measuring.
  - **No leaf light on grass**: the user: it works on the trees, not on grass. The blades are Grama, Tundra and
    Chamisa; Chamisa (and its bloom) now carries grass's transmission (40, under the leaf light's 0.2), so blades
    and Chamisa shrubs keep soft shadows but get no sheen, wrap or glow. (The palette is full, 254 slots: a
    separate blade material would not fit.)
  - **Leaf glow, as 40-advanced-renderer does it** (the user pointed at it): every foliage voxel's sun ray asks
    whether the *voxel* is in sunlight -- from its centre, out 0.95 of an edge along the sun, whichever way the
    face points -- so a leaf seen from its shaded side knows whether the sun strikes its far side. The glow:
    albedo x tint (1.55, 1.05, 0.35) x sun x (pow(view . sun, 3) + 0.22) x a rim gradient across the face (0.35
    in the middle to 1 at the edge, the sun's side brighter: foliageEdgeGlow), strength 0.6. With the shadow
    blur it rides the sun's light (sunRad), so it takes the blurred visibility -- fading across a shadow's edge
    -- and the far field's shadow. Replaces the one-layer rule and the back/toward lobe. Not ported: its
    screen-space density ring (a clump glows less than a lone blade).
  - **...and through the crown, not only at its edge** (the user: strange on the edge voxels alone). More generous:
    a leaf voxel whose sun ray stops in other foliage gets 0.3 x exp(-depth / 1.8 m) of the light. And spread
    in screen space (`shadow_soft`, `sunLight`): the glow's part of the sun's light (its share in gMotion.w)
    takes the better of its own visibility and that of the leaf texels within 1.5 m (the player's) at about its
    depth, 16 taps, nearer counting more, x 2 x 0.5 -- light the edge leaves found carries on into the crown.
    Tint softened to (1.3, 1.08, 0.55) and the rim's floor raised to 0.7 (0.35 drew every voxel's outline once
    the whole crown glowed). Panel: leaf glow spread (m), leaf glow spread amount.
  - **Bright shadows behind shaded foliage**: a sun ray stopped by leaves or grass was let partly through on the
    guess that the sun was beyond them -- under a bigger tree it was not, and a shaded bush cast a bright shadow.
    A sixth segment (same march call site) goes on from just past that voxel: open, the partial light stands;
    more foliage, half; anything else (or starting inside a solid voxel), a shadow. ~+2.5 ms of trace on the
    forest floor.
  - **Trees, specks, trees with distance**: the shape pyramid kept a coarse voxel when 2 of its 8 children were
    solid, decided level by level, so a sparse crown lost its outer layer at every level (by the 1.6 m ring, a
    few blocks); it now carries the filled fraction from level 0 (solid at 20%). And the 3.2 m ring stamped no
    trees at all (`stampTrees = v <= 2`), only blocks of canopy colour, before the far field's crowns: trees are
    stamped there too. Left: the far field's crowns read denser and darker than the voxel trees just inside them
    (tree cell, crown scale to match).
  - **The panel, by subject**: Sun and exposure, Shadows, Leaves, Sky light and AO, Surfaces and normals, Clouds,
    Air and haze, Distance, Display; each feature's switch in its section, the whole list under Every feature
    switch. (The old list's checkboxes had done nothing since S16: a comment had swallowed the assignment.)
  - **Still a barren ring** (the user): tinted by ring (`SANTA_FE_DEBUG_RINGS`: 0.1 m red .. 3.2 m blue, far field
    magenta) it was the 3.2 m ring -- a 4 m juniper is under a fifth of a 3.2 m voxel, so it vanished. A shape's
    level is now solid at a fifth filled or, where even its densest voxel is under that, at 60% of its densest:
    every tree keeps its core at every level.
  - **Clouds too white**: the multiple-scattering octaves went 0.6 / 0.3 a step (the fourth reached as deep as 3%
    of the optical depth: a cloud's heart as bright as its head); now 0.5 / 0.5 -- grey bases, shaded sides. And a
    brightness (0.75; mtnWind.w) on the clouds' and cirrus' light. Panel: Clouds, brightness.

- **S19** (2026-11-23): terrain completeness -- the user: grassy fields without trees, snow on the trees in snow,
  flower fields.
  - **Meadows** (`surface.hpp`): open grassland 1-2 km across (fbm at 1.6 km) on gentle ground (slope < 0.35-0.6)
    from 1950 m to the tundra: cover and understory x (1 - meadow), the ground all grass; the grass carpet 0.38 ->
    up to 0.68 there. The far field's crowns go with the cover.
  - **Flowers**: four one-shade materials -- aster (purple, most: October), lupine (blue), daisy (white),
    paintbrush (red) -- and chamisa's bloom for yellow, in drifts (fbm at 70 m picks the kind). Fields of them in
    the meadows (fbm at 160 m), a scatter in any open grass. In the carpet a flower head tops a stem on 10% of
    grass cells x the field; in the 0.4-0.8 m rings a flower voxel in the grass; from afar a tint (18% of the
    flower's colour) in the ground's colour. The palette was full: water and the flowers take one slot each
    (`oneShade`), five freed by water for four flowers.
  - **Snow on the trees**: the October snowline came down from 3620 m (above the timberline: no tree ever stood
    in snow) to 3380 (`snowlineAt`; November 3200; north faces 250 m lower), into the spruce-fir. trace.frag
    snows on foliage tops from the snowline (`sfSnow.x`, 320 m to full, ragged by +-60 m) or wherever the far
    field's ground under it is snow-white: the tops of leaves and needles white, the sides a little, in clumps;
    snow does not glow. The far field's crowns take snow from the snowline too (they took the ground's, which
    the cover thins).

- **S20** (2026-11-23): microdetail -- the user: render-time sun flecks on snow and rock, sticks and logs, less even
  trees; small, cheap things that make it feel complete.
  - **Sun glints** (trace.frag, SF2_GLINTS): a world cell about 1.5 pixels across (a power of two, so it holds
    still) holds one tiny mirror tilted at random within ~20 degrees of the surface; it flashes the sun
    (x30) when it lies within ~3 degrees of the half vector (sharpness 0.9985). Snow every cell, granite and
    gneiss (metallic, no gloss) 35%, snow on the trees too; out to 300 m; with the sun's light, so shadowed and
    blurred with it; the glow pass blooms it. **Replaced by S21** (its sliders and `SANTA_FE_GLINT` are gone).
  - **Sticks and logs**: small log shapes (0.7-1.7 m, deadwood and bark) placed every 3.75 m under any trees
    (0.045 per m2 x cover); logs twice as common (0.004) and in the pinon-juniper too (silver deadwood, 2-4.5 m).
  - **Trees in clumps**: anywhere in their cell (0.1-0.9 spaced them like an orchard), kept by 0.35-1.65 x the
    cover from a 40 m noise (about 1 on average: the far field's cover still agrees).

- **S21** (2026-11-23): glints and sheen, reworked -- the user: S20's glints were not good. Wanted: a 16 x 16 grid
  where small squares or sections are glossy, actually reflecting the sun with a screen-space reflection and much
  less glow; snow, rocks and leaves a subtle rough (specular-diffuse) screen-space reflection of the sky. All at
  render time, nothing stored, tunable in the shaders.

  | # | Decision | Why |
  |---|---|---|
  | S21a | **The grid is per voxel face**, in its chunk's frame, from where the ray meets the face: 16 x 16 cells. A cell is a mirror alone (snow 4%, rock 1.5%) or as most (55%) of a mirror section of 4 x 4 cells (snow 5%, rock 3%), which flash together | The user's choice: it scales with the voxel and holds still; sections are the "sections" |
  | S21b | **A mirror shows the sun through the sun's shadow ray**, as a disc 1.2 degrees across (the mirrors are not flat) at the sky's own disc radiance (40 x), times its Fresnel (ice 0.03, mica 0.06), **held under the glow's threshold** (1.8 exposed; the glow starts at 2) | A reflected ray toward the sun meets exactly what the sun's shadow ray met, on or off the screen; the cap is "much less glow" |
  | S21c | **What else it reflects, and the sheen, through `reflect.frag`**: the trace pass writes the ray (gGloss, an eighth target: direction, weight) -- the mirror's, or one GGX direction a frame for the sheen; reflect.frag marches it through this frame's gPos and reads last frame's image where it hits, last frame's sky where the screen shows sky in that direction, the analytic sky past the screen; shadow_soft adds it | Screen space, as asked; the sky fallback (the user's choice) because the screen rarely holds what a ray reflects off it |
  | S21d | **Sheen**: snow 0.22, rock 0.12, leaves 0.08, GGX roughness 0.5 / 0.6 / 0.55, a rough Fresnel (at grazing up to 1 - roughness, not 1) | Snow > rock > leaves (the user). The first try (0.6 / 0.35 / 0.25, roughness 0.35-0.5, plain Schlick) mirrored the clouds in the snow at grazing views |
  | S21f | **A rock's shine is its palette glossiness**: the boulders' roles carry their brush's (granite 0.12, slate 0.45, quartzite 0.5, quartz 0.75, mica 0.8, pyrite 0.85; capped at 0.88, under water's 0.9), the terrain's granite 0.15, gneiss 0.3, basalt 0.2, lichen 0.04. Dull rock (shine 0) to shiny (`SF_ROCK_SHINY` 0.6 and up): sheen 0.2 -> 0.9, roughness 0.6 -> 0.25, glint cells 2% -> 12%, sections 3% -> 20%, F0 0.06 -> 0.15. Leaves' sheen 0.2. The renderer tells water (glossiness over 0.9), snow (drawn 0.78 smooth, a little gloss) and rock (drawn fully smooth, or any other glossy entry) apart explicitly | The user: leaves stronger, rock much stronger -- really strong on some rock, a little on the rest. The boulders had been interned with their colour only (no glints or sheen on them at all), and their smoothing byte stays 0 so they are not drawn rounder |
  | S21e | **The tunables are defines** at the top of trace.frag (`SF_GLINT_*`, `SF_SHEEN_*`) and reflect.frag (`SF_SSR_*`); the panel keeps only the on/off switches (sun glints; gloss, now water and sheen) | The user: tunable in the shaders |

  As built: reflect.frag costs 0.13 ms at Baldy (13.2 ms GPU in all). Water keeps its own mirror (the gloss path,
  now water only); snow's old soft sheen there is replaced by S21d. Adding gGloss shifts the samplers of every
  pass that reads the trace's targets and more after them (ao_trace, clouds, shadow_soft, resolve).
  Open: the sun glints have not been seen in a sunlit view yet -- the user to judge them; the sheen brings out the
  snow's terrace risers as faint contours at grazing views.

- **S22** (2026-11-24): a clean image -- the user: the reliance on temporal upscaling, antialiasing and
  denoising is far too prevalent and the image feels unclean; for a voxel look a clean image matters a great deal.
  Today every frame is a half-resolution trace jittered over 16 points, with one random sample each for the
  sun's disc, the diffuse ray, the fog, the cloud march and the sheen, and the resolve averages them over up to
  4096 still frames (12 moving); the traced AO and the clouds keep histories of their own. Only the
  reconstruction (the voxel coverage upscale) is spatial.

  | # | Decision | Why |
  |---|---|---|
  | S22a | **Surfaces keep no history.** The resolve's history is kept only where the pixel shows cloud or sky; the camera jitter is off unless the temporal upscale is chosen (S22b). Reference mode keeps today's accumulation | The user's choice: only clouds and sky; marching clouds clean in one frame costs too much |
  | S22b | **The upscale is chosen per layer.** The frame is composited after the upscale, at window resolution, from layers: the base (albedo x the sky light, emission), the sun (its light), the sun's visibility, the traced AO, the SSAO, the reflections, the clouds. Each layer has an upscale setting: *reconstruct* (the voxel coverage weights), *spatial* (edge-aware, guided by the full-resolution depth and normal the reconstruction writes), *temporal* (jittered, history). Panel: Upscale, one row per layer | The user: settings to toggle reconstruction vs temporal vs spatial per effect, to dictate how reflections, AO and the rest are upscaled. Today the composite happens before the upscale, so one method serves all |
  | S22c | **Shadows are deterministic, at window resolution, filtered with an A-Trous blur.** A shadow pass at the window's resolution: each pixel's point is its ray against the voxel face the reconstruction chose (exact, no second primary trace), one hard ray to the sun's centre, its blocker distance; then an A-Trous filter (4-5 iterations, stopping at depth, normal and plane) whose radius the blocker distance sets (PCSS, as S11). The sun's disc sampling becomes reference-mode only | The user: no temporal at all, fully deterministic at high resolution with an A-Trous blur |
  | S22d | **The traced AO: fixed rays, a spatial denoise, held per voxel face.** Each texel's rays come from a fixed stratified set indexed by its place on its face (the same every frame); an A-Trous filter over the trace's texels (a broad radius: it is the large-scale term); then each face takes the filtered value at its centre, so every pixel of a face agrees. **The SSAO is per pixel**, at window resolution, after the upscale, a fixed kernel | The user: the AO per face is fine as long as the SSAO is per pixel; the ray-based AO spatially denoised because it is concerned with a broader scale |
  | S22e | **The sheen reflects the sun and the sky only.** The sun as an analytic GGX lobe times the sun's visibility; the sky along the reflected direction from this frame's sky and clouds, blurred by the roughness, where that direction is on the screen and shows sky; otherwise one or two fixed rays in the lobe against the analytic sky and the cloud-shadow map. No per-frame GGX sample and no march through the screen for the sheen. The glints (S21a/b) keep their mirror ray through reflect.frag, which was never random | The user: the sheen can be simpler, only the lights (sun, sky), a blurred screen space when it can, one or two rays when it can't |
  | S22f | **Optimized mode has no random samples on surfaces.** The fog's sample point is fixed (an analytic in-scatter over the segment), the leaf-spread and shadow-blur disks are fixed patterns; only the cloud march stays jittered (the clouds keep their history, S22a) | Anything random per frame needs the history this removes |

  Milestones:

  | # | What | Done when |
  |---|---|---|
  | M1 | The layered composite after the upscale; the reconstruction writes full-resolution depth and normal and upscales the layers set to *reconstruct* with its weights; *spatial* (joint bilateral) and *temporal* (today's resolve, per layer) for the rest; the jitter off by default; the resolve's history on cloud and sky pixels only; the Upscale panel | A still image with every layer on *reconstruct* matches today's single frame; moving shows no smear on surfaces; the cost written down |
  | M2 | The window-resolution shadow pass and its A-Trous filter (S22c) | Shadow edges stable and clean while moving; penumbrae widen with blocker distance; cost written down |
  | M3 | The traced AO's fixed rays, A-Trous, per-face hold; the SSAO at window resolution (S22d) | No AO crawl while moving; contact creases per pixel |
  | M4 | The sheen from the sun and the sky (S22e) | Snow, rock and leaves sheen without noise; the glints as before |
  | M5 | The fog and the remaining fixed patterns (S22f); captures and the user's judgement | No surface noise in optimized mode with history off; frame time against S21's 12 ms |

  As built (all five at once, the user: "do all of these asap"):
  - **The layers** (shadow_soft.frag, now the split, at the trace's resolution): 0 the colour without the sun, a
    the sky light's share; 1 the sun's light, a the leaf glow's extra visibility (sfLeafSpread, still worked out
    here); 2 reflect.frag's light, a the hard visibility; 3 blocker distance, depth, motion. A blocky-style cloud
    box is composited here whole and marked (a -1). The clouds are composited in resolve.frag now, per window
    pixel against the window-resolution depth.
  - **The reconstruction** sums every layer through one set of weights: a first loop finds the voxels as before,
    a second gives each texel its share (its voxel's coverage x its weight, plus the background's); per layer
    (sfUpscale) the spatial filter instead -- the bilinear tent held to the front voxel's depth. It writes the
    front face (centre, edge) and the normal and depth at the window's resolution. 0.44 -> 1.3-1.6 ms.
  - **Shadows, S22c changed**: no second shadow ray at the window's resolution. The trace's shadow ray already
    starts at the voxel face's centre, so every texel of a face gets one answer and a window-resolution ray per
    face would give the same one; the reconstruction rebuilds it with the voxels' edges. shadow_prep.frag
    (blocker search, 16 taps over 24 pixels, the penumbra in pixels) then shadow_atrous1/2/4/8 (5 x 5 B3, plane
    and normal stops, each step gated off where its taps would reach past the penumbra). ~0.6-0.8 ms.
  - **AO**: ao_trace's two rays a texel, their directions from a world cell about a texel across (a power of
    two); no history. ao_atrous1/2/4 at its quarter resolution (~0.1 ms). resolve.frag reads it where the
    pixel's face centre is on the screen (if the depth there is that face's) or at the pixel; the SSAO is per
    window pixel, 16 taps on a fixed spiral (no rotation: nothing to average).
  - **Sheen**: the mirror direction, the roughness in gGloss's length (1 + alpha; 1 a glint); the sun's highlight
    an exact GGX lobe in the trace (SF_SHEEN_SUN), under the glints' cap. reflect.frag: the analytic sky, this
    frame's clouds blurred by the roughness (9 taps over SF_SHEEN_BLUR_PIXELS x alpha) where the screen shows the
    direction, two sky directions of the lobe past it. Reflections are darkened by the AO.
  - **Glints, prefiltered** (not planned): without the jitter and the history, cells smaller than a texel
    aliased into sparkle across distant snow. The grid is now taken at the level whose cells are about a texel
    (up to 16 x 16 merged); a merged cell is a mirror if any of its cells would be, as bright as its mirror
    share: the same light, stable.
  - **Clouds**: their own history is the one left; a still view blends 4% a frame (was 12%: the resolve's
    long history had been hiding their noise), a moving one 12%.
  - **The panel**: Upscale -- colour, sun, reflections, shadow (reconstruct / spatial / temporal), the shadow
    filter, the traced AO (per face / spatial / temporal), the AO filter. Optimized mode: all reconstruct, per
    face, both filters, no jitter. Reference mode: colour temporal (its sun disc and diffuse ray ride in it),
    the jitter on. "shadow blur" is now "sun apart": the sun out of the colour, which the filter needs.
  - **Cost**: 15.2 ms GPU at Atalaya, 13.4 at Baldy (13.2 before, S21).
  - **Taken back a step** (the user, after seeing it: far less clean without the history): optimized mode puts
    the colour, the sun and the reflections back through the voxel reconstruction and the temporal resolve (the
    jitter on), as before S22; the shadow keeps its A-Trous filter and the AO its fixed rays and filter, both
    deterministic, and the traced AO is per pixel (spatial), not per face. The settings stay in the panel.
  - **A reconstruction fix**: where the two loops disagreed on a texel at an edge between two voxels (one the
    first loop gave next to nothing, or counted under another layer), its share divided by a sum it was not
    in and blew up into bright squares. Each share is now at most its layer's whole and the shares are
    normalised: an edge reconstructs a little over, never into a flare. (The old single-loop version
    normalised each layer by its own sum and could not do this.)
  - **Flicker under movement** (the user: a lot of temporal instability; restir-pt's upscaler looks flawless --
    take more from it). The reconstruction was already restir-pt's; the resolve now follows restir-pt's rule for
    rule: the motion of the nearest surface in the 3 x 3 pixels round (no trails or flicker along silhouettes);
    a pixel counts as moved when its own motion is over a hundredth of a pixel; the clamp over the full 3 x 3
    neighbourhood (S22's first cut clamped to a cross of 5, too tight a box: the history snapped to each
    jittered frame); still history 64 frames (4096 only where random samples need converging: reference mode),
    moving 16 (was 12); what a moving thing uncovers clamped and cut to 4. And a merged glint cell is now about
    two texels, not one (the jitter moved a texel's sample between neighbouring cells each frame).
  - **Cloud edges flickered** (the user found it: voxel edges against the clouds, not against the sky or other
    voxels). The clouds were composited after the history, per window pixel, by a hard depth test: a
    silhouette's pixel flipped between the voxel's depth and the sky's as the jitter moved, so the cloud
    switched fully on and off there each frame. They are put on at the trace's resolution again, in
    shadow_soft.frag, before the reconstruction (each layer through what they let through, their light in the
    colour): the edge is reconstructed and accumulated like any other colour.
  - **AO in the distance** (the user: reference looks better, mostly in its distant voxels; its diffuse ray
    occludes at every distance). The traced AO stopped at 300 m, its 6 m rays were under a pixel a kilometre
    out, and the SSAO turned itself off once its 0.4 m was under 1.5 pixels. Now: out to 3 km (faded over the
    last fifth), its reach the larger of 6 m and 1.2% of the distance (SF_AO_REACH_PER_M), and the SSAO at
    least 4 pixels across (SF_SSAO_MIN_PIXELS).
  - **AO on the far field** (the user: distant hills still flat -- the far field past the voxel rings had no
    AO at all: its sky-light share is 0 and no AO ray reaches it). A horizon AO on the heightfield, in the trace,
    no rays: 6 directions, in each the highest the ground plus 0.7 of its canopy rises above the point's tangent
    plane at 3 distances (from the larger of 20 m and four pixels' ground, x3 each), the sky over that horizon
    averaged (`sfFarAO`, SF_FAR_AO_*). Off with the diffuse ray (reference mode occludes by tracing). ~0.2 ms.
    Its own switch, SF2_FAR_AO (panel: Sky light and AO, "AO on the far field").
  - **The sky light from the whole sky** (the user: in reference the cloud shadows are much brighter and coloured
    by the clouds and the sky -- the diffuse blue sky light the path trace had). With no diffuse ray a surface's
    sky light was the analytic sky in ONE direction (over its normal, toward the zenith: the darkest blue of it)
    x 0.8, no clouds, no ground. sky_ambient.frag now takes the hemisphere's cosine-weighted mean once a frame
    as an ambient cube (6 texels, 128 fixed directions each, what a diffuse ray that leaves the scene sees: the
    sky, the clouds' light at their layer -- the trace's own sfCloudSky --, the plains below), and the trace
    weighs it by the normal (sfSkyAmbient, SF_SKY_LIGHT 1.0). Snow in a cloud's shadow at Baldy went from deep
    blue to within a little of reference's. 0.3 ms. Switch SF2_SKY_LIGHT ("sky light from the whole sky").
  Open: the user's judgement of the look while moving; the reconstruction's cost (four layers through one
  weight loop, 1.6 ms at Atalaya) is the largest new item.

- **S23** (2026-11-24): the sky in the render, and clouds and fog set up as production systems
  do -- the user: a step in the right direction, but the sky needs more integration: clouds turned to full
  coverage, huge and low, should feel very dark, and came out bright (correctly diffuse, but bright). And more fog
  settings, following how other cloud and fog systems work.

  Why it stays bright: three places still light the scene as if the sky were clear.
  (1) The sky light's cube (S22) sees the clouds through sfCloudSky -- a fixed grey (the sun's light x 0.12-0.22
  plus the clear zenith x 0.6) at most 85% opaque, whatever the deck's thickness. (2) The clouds' own ambient
  (sfCloudLight) is the clear zenith from above and the SUNLIT ground from below (groundUp), so a deck in its
  own shadow keeps clear-day fill. (3) The haze's in-scatter (skyUp x 0.75 + groundUp x 0.5) and the air's
  (mtnAtmosphere: the sun unshadowed) ignore the clouds.

  How others do it (sources: Epic's Exponential Height Fog and Volumetric Cloud references; Schneider, "The
  Real-time Volumetric Cloudscapes of Horizon Zero Dawn" 2015 and "Nubis" 2017; Hillaire, Frostbite 2016):
  - Unreal's height fog: density, height falloff, a second layer (density, falloff, offset), inscattering
    colour, sky-atmosphere ambient contribution, directional inscattering (exponent, start distance, colour),
    start distance, max opacity, cutoff distance; volumetric fog: scattering distribution (phase g), albedo,
    extinction scale, emissive, view distance.
  - Unreal's volumetric clouds: layer bottom and height, tracing distances, "sky light cloud bottom occlusion"
    (the sky light darkened under the layer: a cheap stand-in for clouds occluding it -- exactly (1)), "ground
    contribution" (the shadowed ground lighting the cloud from below -- (2)), aerial perspective start and fade
    on clouds; the material's phase G, G2 and blend, multi-scattering contribution / occlusion / eccentricity
    (our octaves' 0.5 / 0.5 / 0.5, hard-coded), octave count.
  - Nubis: a weather map -- coverage, cloud TYPE (stratus .. cumulus .. cumulonimbus: the height profile) and
    precipitation (denser, darker rain clouds); art-directable, several types at once.

  | # | Decision | Why |
  |---|---|---|
  | S23a | **The sky light sees the real clouds**: sky_ambient.frag marches each of its directions through the volumetric layer (sfCloudMarch, fewer steps), spread over a 64 x 6 target and averaged by a second tiny pass | A thick deck's base is dark and opaque; the stand-in was a fixed grey |
  | S23b | **Overcast reaches every light**: one number a frame, the sun's mean transmittance through the clouds round the camera (the cloud-shadow map, sampled over ~10 km), dims the ground's bounce (groundUp) everywhere -- the clouds' fill from below, the haze, the cube's lower half; the clouds' fill from above is the sky as seen under them (the cube, not the clear zenith) below their middle -- Unreal's "cloud bottom occlusion" | One deck shades the land, so the land throws less light up; the clouds' undersides then darken as they should |
  | S23c | **The haze and the air under the clouds**: the haze's sky in-scatter from the cube; its sun in-scatter and the air's (aerial perspective) times the cloud shadow along the view (at the camera and at the far end, mixed) | Under overcast, distant hills should not glow with clear-day haze |
  | S23d | **Fog settings after Unreal's height fog**: density, height falloff (scale height), base, a second layer (density, falloff, offset), inscattering tint, sky contribution, directional inscattering (strength, exponent / phase g, start distance), start distance, max opacity; albedo | The user: more fog settings, as others have them |
  | S23e | **Cloud settings after Unreal and Nubis**: phase G, G2 and blend; multi-scattering contribution, occlusion, eccentricity (now hard-coded 0.5) and octaves; ambient strength; ground contribution; cloud-bottom occlusion; powder; cloud type (stratus .. cumulus .. cumulonimbus height profile) and precipitation (darker, denser) -- global, not yet a weather map | The same; type and precipitation are what make a storm read as a storm |
  | S23f | **Weather presets**: clear, scattered, overcast, storm -- each a set of the above | Quick to reach a believable dark sky |

  Milestones: M1 S23a-c (the overcast darkens: a full low deck reads dark, captures against the user's);
  M2 S23d (fog panel); M3 S23e (cloud panel); M4 S23f (presets). Cost: the cube's march ~0.3 -> ~0.6 ms
  expected; the rest is uniforms.
  As built (all four, the user: "do all 4"):
  - **The sky's light on the GPU, read back** (S23a/b): sky_dirs.frag (64 x 7: 64 fixed directions round each of
    six axes, each the volumetric clouds marched (sfCloudMarch, half the steps) then the sky, or the land below in
    the clouds' shadow there; row 6 the clouds' shadow at 64 points within 9 km) and sky_ambient.frag (the rows
    averaged, 8 x 1: texels 0-5 the ambient cube, 6 the land's mean sun transmittance). The passes that need it
    bind it and define SF_SKY_TEX_SLOT before mountains_sky.sc (the trace, clouds.frag, reflect.frag, sky_dirs
    itself a frame late); the rest see the clear sky. clouds.frag had no slot left: it now reads the trace's
    gPos and gNormal through fbo 17 (those two alone) instead of all eight. 0.15-0.25 ms. First built as a
    readback to uniforms -- the user: a lag spike every ~10 frames, unplayable: bgfx's Vulkan readTexture
    kick(true)s, waiting for the GPU to go idle, and it ran every fifth frame.
  - **Every light under the clouds**: mtnGroundUp() (mountains_sky.sc) is the land's light thrown up -- the sun
    on it times the overcast, plus the sky's light on it (the cube overhead); the clouds' fill from below, the
    trace's, the plains in mtnSkyFrom and skyColor all use it. The air's own light (aerial perspective, the
    diffuse ray's distance) is dimmed by mtnAirLit(): sfOvercast.y (0.6, "air under clouds") of the overcast.
  - **Overcast decks came out navy at first**: the bases' fill read the cube, which sees the dark deck -- a loop
    that converged to night -- and deep in a thick deck the octaves' exponentials are zero. Now the bases' sky
    fill is the clear sky through the deck above by the two-stream diffusion, 1 / (1 + 0.75 (1 - g) tau) (g 0.85,
    tau along the light march), weighted by "bottom occlusion"; and the same diffusion, minus what the octaves
    carry, adds sunlight leaving the cloud as from a diffusing sheet (t E / pi). Grey, not black.
  - **Fog** (S23d, trace.frag): two exponential layers (the valley haze + sfFog2), a start distance, a max
    opacity; its sky light from the cube (up 0.75, down 0.5, x "fog sky light"), the sun scattered by its phase
    (4 pi HG, mtnFog.w) where the clouds let it at mid-view ("fog sun light", Unreal's directional inscattering),
    through a tint and an albedo. Panel: Air and haze, "height fog (Unreal)".
  - **Clouds** (S23e, santa_fe_clouds.sc): phase G / G2 / blend and powder (sfCloudPhase); multi-scattering
    contribution / occlusion / eccentricity / octaves (sfCloudMS; the old hard-coded 0.5 / 0.5 / 0.5, 3); ambient,
    ground contribution, bottom occlusion, precipitation (sfCloudAmb: rain is 1.8x denser, its sun light x0.5 and
    fill x0.4); type and erosion (sfCloudType: 0 stratus -- towers cut to 0.22 of the layer, a soft base and top,
    the billows flattened into a sheet --, 0.5 cumulus as before, 1 cumulonimbus -- every tower to the top, +0.2
    cover). Panel: Clouds, "kind (Nubis)" and "light (Unreal's volumetric clouds)".
  - **Weather** (S23f): clear (cover -0.6, thinner haze), scattered (the defaults), overcast (cover 0.55, 3.3-5.6
    km, extinction 0.04, stratus 0.15, drizzle 0.25, haze x2.4 and a second layer), storm (cover 0.85, 3-11.5 km,
    0.1, cumulonimbus 0.95, rain 0.85, haze x5, a deep second layer, no sun in the fog, a cool tint). Panel
    buttons by the mode buttons; SANTA_FE_WEATHER=clear|scattered|overcast|storm for captures.
  - **Cost**: 15.1-15.6 ms GPU at Atalaya in all four (the clouds cost less as the deck closes).
  Open: whether the type and precipitation want a weather map (Nubis) after all -- several kinds of cloud in
  one sky -- rather than one global setting; a storm's bright sun disc still shows through the deck in the sky
  view (the disc is drawn regardless of the clouds' transmittance there).

- **S24** (2026-11-24): rendering polish -- the user: the cloud and sun bloom look very poor; the clouds could
  take a spatial denoise instead of a temporal one; the sheen's reflections flicker under movement (the fixed
  rays change), maybe back to random. Then, after a first cut: research how AAA games render realistic clouds in
  real time and take from it.

  Research (sources in the conversation; Horizon's 2015 speaker notes read in full, the rest from abstracts and
  Unreal's reference):
  - Horizon Zero Dawn (Schneider and Vos 2015): Beer's law x Henyey-Greenstein (the silver lining) x a "powdered
    sugar" term (dark edges facing the light, view dependent: it fades across the view relative to the sun) x more
    absorption in rain clouds. Light: 6 samples in a cone toward the sun -- the cone smooths banding and blends
    neighbouring densities, "a nice ambient effect" -- the last far out, for shadows cast by distant clouds; cheap
    density once the ray's opacity passes 0.3 (2x faster). Colour: ambient sky light growing with height, direct
    the sun's colour, then toward the atmosphere by depth. 64-128 march steps; ~20 ms whole, ~2 ms by updating 1
    pixel in 16 of a quarter-resolution buffer a frame, the last frame reprojected.
  - Nubis (2017, 2022): weather map with type and precipitation; temporally upscaled skies (2022 adds a
    non-temporal path for clouds the camera flies through).
  - Unreal: half/quarter-resolution tracing, temporal reconstruction, octaves, two lobes, bottom occlusion,
    ground contribution (S23 has these settings).
  - Red Dead Redemption 2 (Bauer 2019): a world cloud map from the weather, a table of height gradients per type,
    a 2D cirrus layer, raymarched scattering and transmittance.
  - jpgrenier (Stingray, 2016): 256 steps with a Bayer offset over 16 frames; ambient from the sky sampled at a
    bottom and a top altitude; bases darkened by absorption altitude; high-frequency detail sells scale and needs
    samples.

  | # | Decision | Why |
  |---|---|---|
  | S24a | **Bloom: restir-pt's chain** (bloom.sc: five levels down, Jimenez's 13 taps, Karis-weighted first; four up; mixed in at 4%, no threshold). glow.frag keeps only the light shafts | The threshold glow put a smudged halo round every sunlit cloud |
  | S24b | **The sheen samples its GGX lobe again**, a direction a frame; the reflections layer's history averages it; reflect.frag's blur by roughness 48 -> 12 pixels | The user: the fixed mirror direction flickered as the jitter moved the smoothed normal under it |
  | S24c | **Clouds temporal again, as everyone does it**: the march's offset turned per frame (interleaved gradient noise + the frame); a pass of its own (cloud_temporal.frag) reprojects last frame's at the cloud's distance and clips it to this frame's 3 x 3 neighbourhood (mean +- 1.25 sigma: variance clipping), blends 4% still, 15% moving | Tried spatial first (the user's ask): at a quarter of the resolution a filter can only blur -- the capture was soft blobs. Every system surveyed amortises the march over frames |
  | S24d | **Light in a cone, plus one long sample** (Horizon): the light march's samples spread round the sun's direction by a fixed kernel, widening with distance; one more ~3 km out (cheap density) for distant clouds' shadows | Smoother self-shadowing, the ambient blend, and shadows between clouds |
  | S24e | **View-dependent powder and in-scattering**: powder fades to nothing looking toward the sun; the multiple-scattering octaves times an in-scattering probability by depth in the cloud and height in the layer (thin edges and bases gather less), the single scattering not | Dark edges and bases, bright crevices: what reads as volume |
  | S24f | **More march steps** (optimized 28 -> 40) now the frames share them | Detail |
  | S24g | **A longer tone shoulder for Santa Fe** (display.frag): identity to 0.5, then Reinhard on the peak channel (slope 1 at the knee), hue kept; the engine's Khronos Neutral shoulder (0.76 -> 1) put everything over ~3x white at 0.97-1 | Sunlit clouds near the sun flattened to one cream |

  As built: all seven. The first cut of S24c was spatial (cloud_atrous.sc, three A-Trous steps, no history): its
  capture was soft and detail-less; replaced by cloud_temporal.frag (fbo 29, read by reflect.frag and
  shadow_soft.frag). The vertical in-scattering term is a cumulus' (a heap's flat base sees little of its body);
  it fades out toward stratus (type 0), whose base is where the deck's diffused light comes out. Seen: toward the
  sun the cumulus now have grey shaded undersides and inner structure, crisp edges, and the sun a soft glow; the
  overcast preset reads as a grey deck but darker than S23's (the cone and the long sample find more cloud toward
  the sun, so less diffuses through) -- the user to judge. GPU 15.9 ms looking into the sun (clouds 0.75,
  cloud_temporal 0.02).

- **S25** (2026-11-24): light and colour, measured -- the user: colours and brightness are 100% off; look at the
  colour spaces, the tone mapping, the exposure, and the lighting on the clouds and the sky against the ground.

  The colour spaces were right (palette colours decoded from sRGB in the engine, the far field's in
  mountains_far.sc, all shading linear, sRGB out). So the light was measured: SANTA_FE_RAW=1 shows the scene's
  linear light x exposure with nothing else, captured at exposure 0.25 and read back to exposure-1 luminance.
  With the sun behind the camera: sunlit cumulus 0.34 against sunlit dirt 0.33 (a thick cloud reflects ~80% like
  a white surface: ~3x the dirt), the blue sky at 60 degrees up 0.11 (real ~0.6-1x the dirt), a shaded conifer
  0.010 (right, for dark foliage under that sky).

  | # | Change | Why |
  |---|---|---|
  | S25a | **The aerosol 0.15 -> 40** (air haze; slider to 150): ~0.03 optical depth over Santa Fe | 0.15 was an optical depth of ~0.0005, a hundredth of high-desert air: a navy zenith, no white horizon, no glow round the sun, no blue in the far ridges |
  | S25b | **The sky's multiple scattering 0.8 -> 1.6** (mtnAtmos.w) | The rest of the zenith's shortfall |
  | S25c | **The clouds' multiple scattering 0.5 / 0.5 / 0.5, 3 octaves -> 0.8 / 0.35 / 0.6, 5; brightness 0.75 -> 1.0** | Integrated by hand, the old octaves sent ~7% of the sun back toward a viewer with the sun behind, a real deck ~25%. Stronger octaves would make thin wisps glow; S24e's in-scattering probability keeps them to the thick parts |
  | S25d | **The grade to almost nothing**: contrast 1.2 -> 1.0, saturation 1.25 -> 1.0, vibrance 0.4 -> 0.15 | colour-convention.md: a look is an explicit, light choice on top of a faithful image; this one doubled every error under it (the mustard grass, the navy sky) |

  Measured again (sun behind): the clouds 0.64 (2x the dirt), the sky 0.12 at 60 degrees, 0.31 lower down.
  Seen: white cumulus over a lighter sky, earthy ground and natural greens; into the sun a hazy glow (perhaps a
  little pale near the sun); overcast a bright grey day, storm dark. The tone curve stays S24g's. Left: the sun's
  disc still shows through a deck (it is 40x the sun's light; a few percent of transmittance is a white dot).

- **S26** (2026-11-24): faster -- the user: looks great, make it faster.

  Measured with `SANTA_FE_BENCH=preset` (new: the optimized preset alone, 300 frames after 30, and its passes;
  repeats to +-0.05 ms where the old 8-frame window wandered +-0.7), plaza, 1600 x 900, RTX 3070: **14.3 -> 12.3 ms
  GPU**. Before: the trace 7.3 (its primary march ~5, the sun's ray 1.6; every shading feature together under 0.2,
  measured by compiling them all out), reconstruct 1.65, ao_trace 0.7, depth_pre 0.6, clouds 0.55, resolve 0.45.

  | # | Change | Saves |
  |---|---|---|
  | S26a | **The reconstruction learns each tap once**: its first loop keeps each tap's weight and which voxel layer it joined (layers keep the id they were made with as the sort moves them), the second only reads the layers -- it worked every voxel out again (gNormal, gRot, the coverage) | 0.45 ms (1.65 -> 1.2); the image the same within run-to-run noise |
  | S26b | **A lean trace** (trace_lean.frag): trace.sc's body without the diffuse ray, its bounce's sun ray and the haze's traced shafts, compiled out (SF_LEAN); main.cpp draws with it whenever those are off (all of the optimized preset), with trace.frag otherwise (the engine's per-pass `enabled`). The full shader's registers are sized for its every path | 1.25 ms; the same image (SANTA_FE_NO_LEAN=1 for the full one) |
  | S26c | **The sky light every fourth frame** (sky_dirs, sky_ambient; at once when the look changes): it moves with the clouds' drift | 0.27 ms |
  | S26d | **farMax folded into farState's alpha** (the column's highest, 20 m steps rounded up): a texture and a sampler fewer in five passes | -- |

  Tried and dropped (so not tried again):
  - *A finer skip grid for the far march* (8 x 8-texel max heights built on the GPU each frame): the trace got
    slower, 6.1 -> 7.5 ms, and the grid's own pass cost 1.8. The march is long-latency and register-bound; code
    in it costs more than the steps it saves.
  - *Reusing each face's sun ray from last frame* (exact with per-face shading: every texel of a face traces the
    same ray): the optimized preset shades per hit, so no two texels share a ray; nothing to reuse without
    approximating the shadow edges S22 keeps deterministic.
  - *The depth pre-pass at a quarter of the trace's resolution*: depth_pre +0.1 ms and the trace +0.07.

  What is left is the march itself (the engine's voxel traversal and the far field's): the trace 6.2 of 12.3 ms.
  Reconstruct 1.2, ao_trace 0.7, depth_pre 0.55, clouds 0.5, resolve 0.4.

- **S27** (proposed, 2026-11-25): trees that match at every distance, and faster where it counts -- the user: the
  hand-off from near to far trees is not smooth, the far trees' shapes diverge from the real ones, and it must be
  faster.

  Measured first, from the user's view over the forest (`MOUNTAINS_CAMERA=3897.0,-413.7,7.8,5.3984,-0.0187`,
  `MOUNTAINS_SUN=250.0,22.0`; the second bench view beside the plaza): 1600 x 900, **15.6 ms GPU, the trace 8.9**
  (plaza 12.3 / 6.2). Props' sun shadows ~1 ms of it; the rest is mostly the primary march through the canopy. In
  the user's window (~2546 x 2804, 5x the bench's pixels) it is 37 ms, the trace 28.4: the window, more than the
  view, makes it slow.

  Seen in the screenshot -- three kinds of tree, each hand-off visible:
  1. 0.1-0.2 m rings: the trees as made (cone, lit and shaded sides).
  2. 0.4-3.2 m rings: the shape pyramid's coarse voxels, solid at a fifth filled: the cone becomes loose blocks,
     the ground between them (lit, tan) shows through as streaks.
  3. The far field: crowns on a grid, coloured and sized apart from the voxel trees -- smaller, darker, denser.

  | # | Step | Why |
  |---|---|---|
  | S27a | **The forest view as a bench view** (`SANTA_FE_BENCH` takes both) and the trace split by part there | Every change is judged where it is slow |
  | S27b | **A beam hierarchy instead of a ray a texel** (Laine & Karras's beam optimisation taken further; depth_pre is its one-level form today: a ray through each 8 x 8 tile's centre, the trace starting at the nearest of nine). Beams of 16, 8, 4, 2 texels, each a cone over its tile, marched conservatively from its parent's distance, and each ends as one of: *empty* (the tile is sky or the far field), *one face* (a voxel face covers the whole cone and nothing is in front: every texel's hit is that face's plane, exactly, no march), or *mixed* (split). Only mixed tiles at one texel march per texel. A beam wider than a ring's voxel reads the coarser ring: the beam's footprint is the level of detail (S27d). The sun's ray too: a one-face tile's patch swept toward the sun as one beam, all lit or all shadowed, split only at a shadow's edge | Cost follows what is on screen (edges, leaves), not the pixel count: near voxels cost one beam, not 50 rays; with geometry prefiltered to the footprint (S27c, d) nothing is under a texel, so nothing is left for a temporal upscaler to fix. The user: focus rays where needed, far enough that no temporal upscaling is needed |
  | S27b0 | **Measure the room first**: from the G-buffer, the share of trace texels in 2/4/8/16 tiles that are all one voxel face, and the march's steps per texel as a heat map, at the plaza and the forest | The ceiling of S27b's saving, before building it |
  | S27c | **Coarse trees that cover what the tree covers**: each level's solid threshold chosen per shape so its projected area (from the sun and from the side) matches level 0's; its colour the mean of the outer leaves, not of every voxel | The blocks and tan streaks of the middle rings |
  | S27d | **A tree's level by its size on screen**: past where a ring's voxel is under a texel, the march reads the next ring for trees | No sub-texel leaves to step through (the forest's cost), no shimmer |
  | S27e | **Far crowns from the same trees**: the far field's crown cell, height, radius and colour measured from the shape library and the coarse levels, not set apart | The far forest the same forest, the same density and tone |
  | S27f | **A dithered hand-off**: each tree changes level at its own (hashed) distance across a band | No line where every tree changes at once |

  Later, if b is not enough: the march's spare rays sent where last frame's image was least certain (temporal -- weighed against S22's clean image), and a learned upscaler for what the reconstruction cannot rebuild (sub-texel foliage, the far field): FSR 2/3 (open, every vendor, temporal), DLSS (NVIDIA only, its SDK under bgfx), or a small network trained on this renderer's own scale-1 / scale-0.5 pairs with its G-buffer.

  **As measured (S27a, b0, 2026-11-25).** `MOUNTAINS_CAMERA=forest` is the second bench view.
  `SANTA_FE_DUMP_GBUFFER=<file>` (with `MOUNTAINS_CAPTURE`) reads the trace's gPos and gFace back once;
  with `PJV_MARCH_STATS` defined in trace_lean.frag gFace carries the camera ray's and the sun's DDA steps and
  scene visits instead (`trace.sc`), for heat maps.
  - **One-face tiles are few where the cost is.** Hit texels that would still need their own ray in a beam
    quadtree (tiles one plane, nothing in front): plaza 51% at scale 0.5 / 32% at 1.0; forest 86% / 65%. Grazing
    ground squeezes a face under a texel vertically, and foliage is a different voxel every texel. At scale 1.0
    the forest would trace 2.6x the rays it does at 0.5: beams do not make scale 1 affordable there.
  - **Sky: 38% of texels.** Their rays walked every ring to learn nothing. Now when all nine pre-pass rays
    round a texel miss, its camera ray is not traced (`skipDistance` -1): **forest trace 9.2 -> 8.1 ms, plaza
    6.1 -> 5.6**, the skyline unchanged (diffs are the motes' noise).
  - **Empty space in front is ~10%**: starting each ray at its own pre-pass texel's distance (unsafe, a test)
    saved 0.8 of 8.1 ms. That is the most finer beams could save on the approach.
  - **Where the camera ray's steps go** (forest, 25 a texel): 30-100 m holds 55% of them (57 a texel, the
    crowns seen edge-on); the heat map's brightest line is ring 0's edge, where rays that skimmed the ground
    restart on ring 1, and the crowns' silhouettes. **The sun's ray** takes 7 DDA steps but 31 scene visits a
    texel (the props' tree of loose objects, its reach 45 m / sin(sun)): its cost is finding what to test.
  - Forest, features off one at a time (trace 8.2): sun shadows -1.6, props (ring 0's trees) -1.3, leaf light
    -0.8, far trees -0.4; the rest under 0.2 each.

  **Trees (S27c, e; 2026-11-25).** The user: start on the trees.
  - **S27c, coarse trees that cover what the tree covers** (placement.hpp buildMips): a coarse voxel's coverage
    is the share of its block's lines of finest voxels, along x, y and z, that meet the shape; voxels are made
    solid in order of coverage until the level's projected area (both sides, and from above at half weight)
    comes nearest the shape's. Measured on the cached shapes, the old "a fifth filled" kept 0.19-1.25x of a
    tree's side area (a fir at 1.6 m: 0.27x side, 0.18x top -- the specks and tan streaks); this keeps
    0.95-1.1x of the side, the top 0.5-1.0x on the coarsest conifers. A solid voxel's slot is the one its block
    shows from outside the shape (voxels first along a row from any side, by how many sides), and a block with
    nothing seen (a trunk in its crown) takes its most seen neighbour's. In the forest view the middle rings'
    trees keep their cones and colour; cost the same (forest trace 8.0 ms, plaza 5.2).
  - **S27e, the far field's crowns are the voxel rings' trees** (mountains_far.sc mtnCanopyAt, columns.hpp):
    the far atlas carries the two species and their mix (farCanopy's alpha, in place of a mean height) and the
    cover times collectTrees' clumps, from its 100 m footprint, nothing on rock or water (farColour's alpha);
    the shader hashes the 9.9 m tree cells as trees.hpp does (`mtnTrees.x` the seed), so each crown stands at its
    tree's trunk, of its species, size and height, kept by the same test (growTree's reach a species, measured:
    juniper 0.53 of its height, pinyon 0.42, ponderosa 0.22, fir 0.19, spruce 0.16, aspen 0.20). Each species'
    crown is its shape's (the library's 99th-percentile radius, 0.19-0.57 of the height, and its top a - b d^p
    fitted to the shapes' azimuth-averaged tops: cones for fir and spruce, ponderosa round-shouldered, the
    woodland's domes flat) -- they were 0.45 of the height for all, capped by the cell, two shapes. The column
    skip's maximum is the tallest tree a texel's species can grow (1.6x). Across the hand-off (a raised view over
    the forest) the two sides' mean colour now agree (luminance 101 far, 97 voxels); the far side is smoother
    (its crowns a heightfield, sd 14 against 22).
  - The orange specks through the forest at every distance are the understory's gambel oak in autumn colour
    (trees.hpp: `RedSoil` half the time), not a level's error: in every capture with leaf light or glints off.
    `SANTA_FE_OFF2=<bits>` turns the second flag set's features off for such checks.
  - **S27f (a dithered hand-off) not done**: the march takes each ring's box as the whole truth inside it, so a
    tree cannot hand over by its own distance without the traversal consulting the next ring where a ring is
    empty. Left with S27b and d.

  **The ring-0 edge and the sun's object lookups (2026-11-25).** The user: get to work on both.
  - *The ring-0 edge*: the heat map's line is rays skimming the ground the whole width of ring 0 (a tree march
    in every terrain chunk they pass over, each inside its content box) before they hit just inside ring 1;
    the grid march itself is cheap (ring 1's cells over ring 0's hole are empty visits). The lever is where those
    rays start. Tried and dropped: the four pre-pass rays round a texel instead of nine (trace 8.0 -> 7.7 ms) and
    a tighter margin (0.985 t - 0.3 m for 0.96 t - 2 m; -0.15 more): together the image lost thin geometry in
    blocks and the far skyline (a hill between four sky samples taken for sky; >40 levels off on 7254 pixels of
    the forest against ~1000 of noise). Nine rays and the old margin stay; what is left of this line needs the
    ground coarser for a grazing ray (its footprint along the ground, not its distance), a traversal change.
  - *The sun's object lookups*: a sun ray (segment 1, and 5 past foliage) pops ~26 nodes of the props' tree a
    texel (49.5 visits with props, 23.4 without; DDA steps 11.4 / 7.1), 0.9 ms of the forest's trace (8.0 -> 7.1
    with SANTA_FE_SHADOW_PROPS=0). Its reach does not matter (1000, 40, 15 and 5 m all 8.0 ms): the work is
    where the ray starts, inside the overlapping content boxes of the trees round it -- the props' tree already
    holds content boxes, not cubes. Tried and dropped: an any-hit query for the sun's rays (stop the props' walk at
    the first hit, skip it after a terrain hit): 8.00 -> 7.98 ms, as the engine's own note found on the gallery.
    Left: a sun-space map of the props' occlusion (first foliage and first solid along the sun, ~0.12 m texels
    over ~120 m round the camera), traced once and kept while the sun and the near rings stand still, so a pixel's
    sun ray marches the rings only and reads the props from the map: up to the 0.9 ms, the edges voxel-sharp.

  Open: conservative beams need an any-child occupancy per coarse voxel (the rings' own voxels are thresholded at a fifth filled, so they can miss what a finer ring holds) -- a bit beside each, or a dilated march. And whether S27d can be done in the march (a ring's lookup per step) without the latency S26 found code in it
  costs; else in the bake (the ring holds the coarser tree where it would be under a texel from the rings' usual
  distance).

- **S28** (2026-11-25): a realtime preset -- the user: a playable realtime version on this GPU; 60 fps at about
  1080p, and it may give up voxel range, effects, internal resolution and use temporal reuse.

  `SANTA_FE_WINDOW=WxH` sets the window (a bench now asks for exactly it, as a capture does: before, a tiling
  window manager could have fitted it -- it had not: the old numbers were at 1600 x 900), and the bench prints the
  window and the trace's size. `SANTA_FE_MODE=realtime` (and the panel's Realtime button) is the optimized preset
  with the cuts below; `SANTA_FE_BENCH=preset` measures whichever mode starts, and `SANTA_FE_SCALE`,
  `SANTA_FE_OFF` and `SANTA_FE_OFF2` apply in the bench too, to price a cut. `SANTA_FE_FRAMES=1` logs every
  frame's wall and GPU time and prints percentiles and the frames over 16.7 / 33.3 ms at exit (with
  `MOUNTAINS_FLY`, what streaming does to the frame).

  At 1920 x 1080 the optimized preset is 17.2-17.8 ms (forest; trace 9.5-10), 15.5 (plaza). Priced there (two long
  runs each, +-0.3 ms): render scale 0.42 -2.4 ms, 0.375 -4.1; leaf light -1.1; AO -1.15; and at 0.42 motes and god
  rays -0.4, glints and the far AO -0.3, smooth ground and bevel -0.3, cloud detail and cloud shadows ~0. The far
  crowns are not the far-trees switch's cost (their mean height instead of crowns was 1 ms slower); their cells
  are now passed on position before their species and size are hashed.

  | View, 1920 x 1080 | optimized | realtime (scale 0.4; no motes, god rays, glints, far AO) |
  |---|---|---|
  | forest | 17.2-17.8 ms | 13.7 (trace 7.1) |
  | plaza | 15.5 | 12.4 (5.1) |
  | raised over the forest (250 m) | -- | 16.0 (8.8) |
  | aerial | -- | 14.5 (8.7) |
  **Noisy in motion** (the user, watching a flight). Measured as a frame in motion against the same pose still
  and converged (64 samples), below the horizon (`MOUNTAINS_FLY`, `MOUNTAINS_FLY_SECONDS`: a flight now starts
  once every ring has generated -- the first measurements caught a half-loaded world, the coarse rings' trees
  standing in for near ones not yet streamed; the capture logs its pose; `MOUNTAINS_CAPTURE_FRAMES=n` saves the n
  frames after it, though bgfx's screenshots are not one a frame; `SANTA_FE_NO_JITTER` for measuring). Still, the
  resolve builds the 0.4-scale trace up to the window over 64 jittered frames; moving, it kept 16 clamped into the
  window's 3 x 3 -- about one trace texel at that scale -- so the history was held to this frame's jittered sample
  and the detail the jitter built flickered away; and bilinear history blurred a little every frame. Now (resolve.frag):
  the neighbourhood 3 x 3 a trace texel apart (`sfTemporal.w`), variance clipping in YCoCg (mean +- 1.25 sigma,
  within the samples' range, clipped toward the centre, not per channel), the history by Catmull-Rom. At 5 m/s:
  error 4.65 -> 4.17, pixels off by > 20 levels 2.8% -> 2.1%; streaming kept up (nothing pending in flight). The
  still bench is unchanged (forest 13.7 ms, plaza 12.0).

- **S29** (2026-11-26, in progress: S29a-c built, S29f first pass; S29d's limb darkening built, S29e open): sunset, and an atmosphere that is correct and complete -- the user, at
  `MOUNTAINS_CAMERA=16.0,-2.3,760.6,-6.0877,-0.0787 MOUNTAINS_SUN=250.0,1.4`: a real issue, likely the cloud
  shadows with the sun near the horizon.

  What the sky (mountains_sky.sc) does now, against what a sunset needs:

  | | Now | Missing |
  |---|---|---|
  | Shape | Flat exponential layers (air 8 km, haze 1.2 km); the horizon's rays 400-1000 km through them | A sphere: a level ray's airmass is ~38, not 125; the earth's shadow and the Belt of Venus opposite a low sun |
  | Sunlight in the air | One colour for the whole ray (`mtnSunAir`, 6 km above the camera; CPU Kasten-Young airmass) | Each point's own sunlight (its path to the sun): a low sun's glow red at the horizon, yellow, then blue; the air beyond the terminator unlit |
  | Ozone | In the sun's colour only | Its absorption along the sky's paths (Chappuis band: the twilight zenith stays blue) |
  | Multiple scattering | A flat isotropic share | Hillaire 2020's multiple-scattering table (isotropic, but by height and sun angle) |
  | Below the horizon | The sun's light 0: the sky black at once | Twilight: the upper air and the clouds still lit after the land is not |
  | Clouds' sunlight | `mtnSunAir` for all of them | Their own height's: tops lit pink after the land is in shadow |

  | # | Step | Why |
  |---|---|---|
  | S29a | **Cloud shadows drawn from the sun** (built) | Below |
  | S29b | **Hillaire 2020's sky** (as Unreal's SkyAtmosphere): a spherical planet (6360 km ground, 6460 km top; ground at y 0, the camera's altitude its y), Rayleigh (5.802, 13.558, 33.1 e-6 /m, 8 km), Mie (3.996e-6 scattering, 4.4e-6 extinction /m, 1.2 km; Cornette-Shanks phase, g 0.8), ozone (0.650, 1.881, 0.085 e-6 /m absorption, a tent 10-40 km peaking at 25 km). Tables: transmittance (256 x 64, when the air changes), multiple scattering (32 x 32, likewise), sky view (192 x 108, every frame: the sky's light by direction), aerial perspective (32 x 32 x 32 froxels to ~32 km, every frame, as 2D slices). Santa Fe's haze settings and fog layers stay, on top | The model the field uses for exactly this: correct at every sun angle including below the horizon, cheap per pixel (lookups), and it replaces the CPU airmass |
  | S29c | **The sun's light from the transmittance table** everywhere it is used: the land (by its altitude), the clouds (by each sample's), the haze shafts, the sky light cube | The land goes dark at its sunset while the clouds and the peaks still glow |
  | S29d | **The sun's disc** with limb darkening and its colour from the table, and the earth's shadow on the clouds (their light march's sun below the horizon at their height) | The disc a hard white spot now |
  | S29e | **A twilight check**: the sun at +5, +1.4, 0, -2, -6 degrees from the user's camera, captures and the sky's luminance against measured twilight (a civil twilight's zenith ~1-10 cd/m^2) | Calibrated, not by eye |

  | S29f | **The night** (the user: a real night sky, stars and the moon): exposure that adapts to the light on the land; the moon, its phase from its angle to the sun, the key light once it outshines the twilight; stars, the Milky Way and airglow behind the air | A night that can be seen, lit by what lights it |

  **S29a, cloud shadows drawn from the sun.** At 1.4 degrees a point of the land projects to the cloud base ~160 km
  off along the sun, far off the 32 km map laid on the ground; it then took a 6-sample march, jittered, over the
  first 12 km of a ~100 km path through the layer: blotches of shadow, and noise. The map is now drawn from the sun
  (santa_fe_clouds.sc `sfSunBasis`, the same frame in main.cpp): a texel is a line toward the sun on the plane
  square to it, its transmittance marched in 48 steps from where it rises through the base to where it leaves the
  top, over the curved earth (`sfAltitude`: y plus the fall of the ground away from the camera, 0.2-1.8 km at
  50-150 km). A point of the trace reads its line; past the map, the mean over the land round the camera
  (sky_ambient's texel 6). At 22 degrees the shadows are as before; at 1.4 most of the land is in the clouds'
  shadow, lit through their gaps toward the horizon.

  **As built (S29b-f first pass, 2026-11-26).**
  - *S29b*: santa_fe_atmosphere.sc (the model, the tables' mappings, the lookups) and atmosphere.frag (one RGBA32F
    atlas, 1024 x 224, every frame: transmittance 256 x 64, multiple scattering 32 x 32, sky view 192 x 108, aerial
    perspective 32 x 32 x 32 to 128 km; each region from last frame's others, settled in three frames,
    `sfSunTop.w`). Bound as texture 54 to the trace (stage 8), clouds (8), sky_dirs (7), reflect (13);
    mountains_sky.sc takes the sky, the air in front of things and the sun's light from it where a pass has it
    (`SF_ATMO_SLOT`), the flat layers elsewhere. The haze is constant below SF_HAZE_FLOOR (1.7 km): extrapolated to
    sea level a grazing path 400 km off had an optical depth of ~26. Reads are held to a region's texel centres (a
    streak in the anti-sun direction was the next region bleeding in). The CPU sun (`throughAir`) marches the same
    model. At 22 degrees the image is as before, the sky a little bluer.
  - *S29c*: the land's sun, the leaves', the sheen's and the bounce's from the table at their own point
    (`mtnSunAt`); the clouds' at each sample (`sfCloudSun`). At -2 degrees the land is dark and the clouds' sunward
    sides and bases red.
  - *S29d*: the sun's disc limb-darkened (Neckel and Labs), its colour from the table.
  - *S29f*: `throughAir`, `twilightLux` (the light on the land by the sun's elevation, almanac values), the moon
    (`moonAzimuth`, `moonElevation`, `SANTA_FE_MOON`; 0.27 lux full, Allen's phase law), `keyLight` (the moon is the
    renderer's "sun" once the sun is past -4 and the moon lights the land more than the twilight), `adaptedExposure`
    (the day's exposure x (land's light at 22 deg / now)^0.8, capped 3e5; `autoExposure`), and in the sky
    (`sfNightSky`, `sfMoonDisc`): stars on a 160-a-face cube in the celestial frame (latitude 35.7), magnitudes 1-7.5,
    a texel's Gaussian; the Milky Way; airglow by van Rhijn; a lit, phased moon with maria. Uniforms `sfMoon`,
    `sfSunTrue`, `sfNight`.

  Open (seen in the last captures, not yet fixed):
  - Moonless (-20): near black, few stars -- the exposure cap (3e5) is too low for 0.0014 lux; 2e6 is the next try,
    and the stars' brightness to judge by eye.
  - -6: a jagged band along the horizon where the sky meets the far ridges (the aerial-perspective froxels against
    the sky view, or the sky skip).
  - A low moon through the haze: its forward scattering glows brown, as a low sun's would, but the cloud shadows in
    it are blocky rectangles (the haze reads the moon-space cloud map's 125 m texels).
  - The key light swaps from the sun to the moon at a step; no scotopic (night) desaturation yet; S29e's calibration
    against measured twilight not done.

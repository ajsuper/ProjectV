# Santa Fe: a quality reference

Status: **in progress**, 2026-11-23: S0-S2 built, S3 first pass, S4 optimized mode, S5 game scale + placements + character, S6 upscaler + boulders, S7 rock as boulders + generation speed, S8 relief everywhere + scale 1.5, S9 gentler cirques + living rock, S10 blocky distance, S11 stable reconstruction + shadow blur + clouds as their own pass, S12 soft quarter-res glow + blocky style, S13 box clouds rebuilt like voxels + tunable glow, S14 cloud-shadow map, S15 normals group + separate blocky toggles, S16 atmosphere: leaf SSS, motes, light shafts, S17 leaf light + traced AO, S18 cheaper shadows and AO, leaf glow only through one layer, S19 meadows, flowers, snow on trees, S20 glints, sticks, clumps. A new example, `examples/56-santa-fe` (target `santa_fe`), forked from
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
    blurred with it; the glow pass blooms it. Panel: Surfaces, sun glints / brightness / sharpness;
    `SANTA_FE_GLINT=bright,sharp`.
  - **Sticks and logs**: small log shapes (0.7-1.7 m, deadwood and bark) placed every 3.75 m under any trees
    (0.045 per m2 x cover); logs twice as common (0.004) and in the pinon-juniper too (silver deadwood, 2-4.5 m).
  - **Trees in clumps**: anywhere in their cell (0.1-0.9 spaced them like an orchard), kept by 0.35-1.65 x the
    cover from a 40 m noise (about 1 on average: the far field's cover still agrees).

# restir-pt — ReSTIR PT at a sixteenth of the pixels, upscaled

`--renderer restir-pt` (`--restir-gi 2`: paths for a quarter of the window's pixels instead)

**As built now: the cheap configuration.** The diffuse and specular *look* of the materials, not sharp
reflections: the paths see every surface's glossiness capped at 0.5 (`RESTIR_GI_MAX_GLOSSINESS`), take two
bounces, and trace coarse past 64 voxels (their sun past 16); validation and random replay are compiled
out. Mirrors -- glossiness 0.9 and up (`RESTIR_MIRROR_GLOSSINESS`) -- get a screen-space reflection
instead. About 1.2x restir's frame (below). Everything in the sections after that describes the full
machinery, which the defines in `restir_path.sc` / `restir_trace.sc` / `sample.frag` turn back on.

Three-bounce paths through the scene's materials — glossy, metallic, mirror — traced for a sixteenth of the
window's pixels and then **shared**: each pixel's path is resampled against last frame's and its
neighbours' (ReSTIR PT: Lin et al. 2022, *Generalized Resampled Importance Sampling*), the shared ones
reconnected where that is safe and re-traced where it is not. The diffuse and specular light are then
denoised apart. The camera rays are traced at half the window's resolution each way and rebuilt to full
by the voxel renderer's reconstruction, then accumulated over frames.

It replaced `restir` (ReSTIR GI: one Lambertian bounce reused by reconnection), which it began as a copy
of. Plan and delivery log: `docs/plans/restir-pt.md`. Its reference is [pt-reference](../pt-reference/).

## The passes

| # | Pass | Resolution | What it does |
|---|---|---|---|
| 1 | `gbuffer` | ½ × ½ | Camera ray and one sun shadow ray (at the sun's centre: no noise), handing on the sun's visibility and its blocker's distance, the material (glossiness, metallic), and the hit face and chunk rotation for the reconstruction |
| 2 | `surface` | ¼ × ¼ | Each GI pixel's visible point — one render texel of its block, a different one each frame — packed into two texels, so the path passes read two inputs rather than the G-buffer's six |
| 3 | `sample` | ¼ × ¼ | A new path from the visible point; last frame's reservoir for the same surface, its path validated (a quarter of them a frame) and shifted here: temporal reuse |
| 4 | `spatial` | ¼ × ¼ | Five neighbours' paths shifted here: spatial reuse, and a visibility ray for a borrowed reconnection. Resolves to the indirect diffuse light (divided by the diffuse albedo) and specular (divided by the lobe's directional albedo); hands its reservoirs on as next frame's temporal history |
| 5 | `gi_accumulate` | ¼ × ¼ | Temporal accumulation of both: the diffuse 12 frames at most, the specular 2 (a mirror) to 12 (rough), with luminance moments |
| 6–9 | `atrous1/2/4/8` | ¼ × ¼ | À-trous filter at steps 1, 2, 4 and 8, both lobes, edge-stopped by normal and plane; the diffuse by a loose luminance test against its 3×3 mean, the specular by the lobe's reach instead |
| 10 | `ssao` | ½ × ½ | Subtle screen-space AO, in a fixed 4×4 rotation pattern |
| 10b | `shadow_soft` | ½ × ½ | The sun through the BSDF — diffuse and highlight — with a soft shadow (PCSS-style, in screen space). The lobe is widened by the sun's disc, so a mirror shows a sun the disc's size |
| 11 | `composite` | ½ × ½ | Edge-aware upsample of both lobes; the diffuse times the texel's diffuse albedo, the specular times its specular albedo: the demodulation put back at full resolution |
| 12 | `ao_apply` | ½ × ½ | The SSAO on the indirect light, the sun's light added, the distance fog, the views (G) |
| 13 | `reconstruct` | window | The voxel renderer's reconstruction, copied: voxel by voxel, antialiased |
| 14 | `resolve` | window | Temporal accumulation (TAA): the jitter's samples and what noise is left, reprojected by the G-buffer's motion vectors |
| 15–17 | `meter`, `exposure`, `dof`, `display` | | The post (P) and the engine's display transform, as restir had them |
| | `motion_blur` | window | After the depth of field: a very subtle blur along each pixel's motion (M; part of P) |

Every ray goes through the engine's `raySceneIntersectFrom` from one call site per pass, in a loop whose
bound is a uniform (`restirPath`: 17 rays in `sample`, 12 in `spatial`). Transparent materials draw as
solid. Secondary rays are exact — restir's coarse bricks shrank the material room's skylight and window
and darkened its indirect light by a fifth, and cost little to give up (below).

Some passes bind single textures (`resourceTexturesIDs` in `render.json`) rather than whole framebuffers:
a pass that traces has nine input slots (the scene takes the other seven), and a path reservoir is five
textures.

## The cheap configuration

| | Full | Cheap (now) | Why |
|---|---|---|---|
| glossiness the paths see | as stored | capped at 0.5 (GGX alpha 0.25) | every vertex is rough, so every path reconnects at x2 and no shift re-traces anything |
| bounces | 3 | 2 (`PATH_BOUNCES`, pt-reference too) | restir's; the third cost half again |
| secondary rays | exact | coarse past 64 / 16 voxels | restir's; costs the material room about a fifth of its indirect light |
| random replay | `RESTIR_REPLAY 1` | compiled out | never used under the cap; its state on every ray cost occupancy. The spatial pass traces one visibility ray |
| validation | `RESTIR_VALIDATE 1` (and `RESTIR_VALIDATE=1`) | compiled out | measured no benefit |
| mirrors | the paths | screen-space reflection | the cap blurs them |

The camera ray keeps the real glossiness: the sun's highlight (`shadow_soft`) and the mirror test use it,
and the composite multiplies the paths' specular back by the capped lobe's albedo, as the paths saw it.

**Mirrors** (`composite.frag`): the view ray reflected about the texel's normal, marched through the
G-buffer in 32 geometric steps out to 1.5x the surface's distance, refined by bisection; a hit's light is
read from last frame's image through last frame's camera, faded towards the screen's edges. Where it
misses -- off the screen, which is most of what a mirror facing the camera shows, behind a surface, a back
face -- the paths' blurred reflection stays. So a mirror reflects what is on screen sharply and the rest
softly.

## Final render, and light shafts

**P switches between the final render and the raw path trace.** Off, the image is what the paths and the
sun light: no post (vignette, exposure, grade, depth of field), and none of what is laid over the light --
the distance fog (O), the light shafts (L), the SSAO. On, those keys toggle each part. U, the soft sun
shadow, is lighting and stays on its own key.

**Light shafts** (`shafts.frag`, GI resolution; L, off by default -- subtle except looking into the sun): the sun's light scattered towards the camera by the
air, in world space, so a shaft comes down through a skylight whether or not the sun is on screen. One
coarse shadow ray per GI pixel per frame, from a point on its view ray drawn from the air's falloff
(`exp(-s / 48 finest voxels)`: open sunlit air is a light haze, not glare), so the estimate's only noise is
whether that point sees the sun. Henyey-Greenstein phase, g = 0.6: shafts glow most looking towards the
sun. Accumulated over 16 frames through last frame's camera, and upsampled in `ao_apply` with a 4x4 tent
weighted by view-ray length, so a shaft does not bleed over what stands in front of it. Added after the
fog. 0.04 ms on Lee Chemicals. Tuning: `RESTIR_SHAFT_*` in main.cpp (strength, falloff, phase, history).
Emissive voxels cast no shafts.

## Paths, and how they are shared

From the visible point x1 a path takes up to three bounces (x2 … x4), each direction sampled from the
BSDF (`include/pjv_bsdf.sc`, shared with pt-reference and the scene editor), with next-event estimation
to the sun at every vertex after x1, MIS-weighted against the bounce finding it, and emission wherever it
meets it. x1's own sun is the direct light's (`shadow_soft`). What a path carries is kept with x1's BSDF
left out, so whichever pixel reuses it multiplies its own in — and splits it into diffuse and specular.

A **reservoir holds a path** (`restir_common.sc` documents the layout), not a point:

- its **seed**: each vertex draws from its own stream, so the path can be re-traced decision for decision
  from another visible point — **random replay**;
- its **reconnection vertex x_k**: the first rough vertex (GGX alpha over 0.2: glossiness under ~0.55)
  more than three voxels from a rough vertex before it — the sky counts as one — with its normal,
  material, emission, whether it sees the sun, and the light the rest of the path brought back to it
  along w_k. That direction is sampled cosine-weighted rather than from the BSDF, so the light reflected
  towards any new x_{k-1} is just π f L, with no density that depends on the old incoming direction;
- the weights M and W, and the visible point it was formed at.

A **shift** (the hybrid shift) moves a path to another pixel: replay x1 … x_{k-1} with the stored
random numbers, then reconnect to x_k, multiplying by the reconnection's Jacobian (cos / d² at x_k, new
over old; outside 1/8 … 8 it is refused). On a diffuse scene every path has k = 2 — reconnect straight
from the new x1, nothing to replay: ReSTIR GI's case. A diffuse wall seen in a mirror has k = 3: the
mirror bounce is replayed, the wall reconnected. A path of glossy vertices only (k = 0) is replayed whole.
A shift fails if the replayed path would reconnect earlier, or ends sooner, or the reconnection is no
longer rough or far enough: the shifted path must be one the other pixel could have made.

**Target** at a pixel: the luminance of the full contribution, its own BSDF included, so it is
view-dependent and re-evaluated by every shift. **Weights** (the cheap configuration): the generalized
balance heuristic -- each candidate's confidence-weighted target in its own domain over the sum of every
candidate domain's, its path shifted to each (`ptReconnectTarget`: arithmetic, since every path
reconnects at x2) -- in both the temporal merge (two domains) and the spatial one (up to eleven). A
pixel with no history of its own (uncovered, or new on screen) reuses from ten neighbours, twice as far,
instead of five. (The full configuration keeps 1/M weights with M counting only shifts that worked.)

**Empty reservoirs count.** Most paths in a closed room come back dark: their reservoirs are empty (W =
0), and that is a sample too -- "nothing here", with its confidence. Both passes used to skip them, which
weighed the bright paths as if dark ones never happened. Spatial reuse alone came out about 4x too bright;
with the temporal history diluting it, the image settled correctly but anything without a history --
an uncovered area, a fresh start -- came in up to 3.5x too bright and faded over a hundred frames (the
image resolve's running mean holding the flare). With the empty reservoirs counted, an area uncovered by a
sidestep is within 1-2% from its first frame (`GALLERY_STEP_AT`: 10% of pixels more than a quarter off
before, 0.6% after).

### Validation: light that changed without being told

A reservoir caches light (L, and whether x_k sees the sun). When the sun moves the renderer is told, and
every history starts over. For any other change — nothing in the gallery, a lamp or a door in a game —
`sample` re-traces the suffix of a quarter of the history's paths each frame (each in turn), from x_k with
the path's own random numbers. On an unchanged scene this comes back exactly the same: images with and
without validation are bit-identical (the direction and normal it starts from are rounded to the
reservoir's packing before the path first uses them, or it would not). Light that changed by more than 10%
replaces the cache and drops M to 1. `RESTIR_VALIDATE=0` turns it off.

Measured with `RESTIR_UNTOLD=1` (the sun lowered 14° without telling the renderer) and the indirect light
alone, the fraction of the change not yet shown:

| | +16 | +64 | +256 frames |
|---|---|---|---|
| SanMiguel, told | 0.51 | 0.19 | 0.05 |
| SanMiguel, untold, validation on | 0.88 | 0.44 | 0.03 |
| SanMiguel, untold, validation off | 0.88 | 0.44 | 0.03 |

Validation makes no difference here: what holds an untold change back is the image's own histories (the
resolve's 64 frames, the GI's 12), which only a told change resets, not stale reservoirs — M is capped at 4
and new paths displace old ones within a few frames. It costs about 0.5 ms on Lee Chemicals; it would
matter for a scene whose paths keep their reservoirs longer.

## Against the reference

Measured on the full configuration (exact rays, three bounces, no cap), before the cheap one. The cheap one
matches on rough surfaces less the coarse rays' fifth, and by design not on glossy ones. The material room (`--scene material-room`), linear light, region means over pt-reference at 600
frames (restir-pt at 300, post, fog and SSAO off):

| Region | No reuse | ReSTIR PT, 1/16 | ReSTIR PT, 1/4 |
|---|---|---|---|
| the mirror (the room seen in it) | 0.97 | 1.06 | 1.00 |
| the wall above it | 1.01 | 1.03 | 1.00 |
| glossy floor, far | 0.99 | 0.99 | 0.94 |
| glossy floor, in shade | 1.00 | 0.97 | 0.97 |
| glossy floor, sunlit | 1.00 | 1.00 | 1.00 |
| white cube | 0.94 | 0.92 | 0.94 |
| blue block (semi-glossy) | 0.96 | 0.95 | 0.95 |
| whole frame | 0.99 | 0.98 | 0.98 |

What took it there, in the order found: exact secondary rays (coarse ones: 0.75–0.8 on most surfaces
lit only indirectly), M counting only shifts that succeed (reuse at 0.6–0.8 before), and the à-trous luminance test
taken against the 3×3 mean — and none for the specular — rather than the noisy centre (the filter alone had
taken the mirror to 0.54 without reuse). The diffuse filter still costs the white cube a few percent.

**The mirror at 1/16** is the right brightness but mottled: one path for each 4×4 window pixels, seen in a
mirror, is all a reflection gets, and the filter may only spread it a GI pixel or so. At 1/4
(`--restir-gi 2`) it reads clearly. That is the plan's first open question answered: 1/4 is the floor
for specular.

## The look

- **The light** is pt-reference's (`sharedShaders/pjv_path_light.sc`): a sky of its own -- a deep zenith,
  a bright band low on the horizon, the sunset's warmth gathered towards the sun while it is low, the
  sun's aureole, and below the horizon the haze carrying on rather than a black ground -- and the sun a
  disc 0.02 radians across each way. The camera also sees clouds (fractal noise on a plane, lit by the
  sun with a silver lining towards it, hiding the disc where thick); they do not light the scene, which
  would only add noise to the GI. pt-reference shows the same.
- **TAA**: every render texel is traced through a different point of itself each frame (a 16-phase Halton
  sequence over the whole texel, so over the 2x2 window pixels it covers and within each), and the
  resolve's history averages them -- supersampling while still, and a voxel thinner than a texel is found
  part of the time instead of always or never. (Before, each texel cycled through the four window-pixel
  centres it covers: a still view repeated the same four points, and the distance aliased.)
- **Colour** follows the engine's convention (`include/pjv_color.sc`, `docs/plans/color-convention.md`).
- **The post** (`display.frag`, on by default; P turns it all off), each part in linear light before the
  tone curve, each the identity when off:

  | Key | Part | What |
  |---|---|---|
  | V | vignette | 15% darker in the corners, flat over the middle |
  | E | auto-exposure | the frame's log luminance metered at 64x64 (centre-weighted), the exposure that brings it 80% of the way to 0.08, held to -2..+3 stops and adapted over time |
  | B | grade | contrast ×1.08 about middle grey, saturation ×1.10, a slightly warm white |
  | K | depth of field | autofocus on the view's centre; the far distance blurs 1.5 px at 1080 lines |

- **The scene's own**, outside the post: U the soft sun shadow (the disc's penumbra, 4% of the blocker's
  distance), O the distance fog: height fog, densest at the scene's lowest point and thinning over 0.15
  of its length upwards, integrated along the view ray exactly -- no onset, no edge: a faint haze that
  builds (about 45% at its length, 1.6 scene radii held to 300..3000 units, at most 90%) -- towards the
  air's colour in that direction (the horizon's haze and the aureole), so the far distance melts into the
  sky it stands against. (Before: a hard start at a tenth of its length, then a steep rise.)

## Stable at a distance

The camera rays are jittered, and where a pixel spans several voxels each frame's ray lands on a different
one. Every test that asks "is this the same surface as last frame?" therefore scales with the pixel's
width at that distance (`restirSameSurface`, the denoiser's edge-stopping), and the resolve keeps a still
camera's history unconditionally — as restir did; on Lee Chemicals that took the shimmer from 23% of
pixels changing between still frames to none.

## Keys

| Key | |
|---|---|
| **G** | The image / indirect light / direct light / ambient occlusion / indirect diffuse / indirect specular |
| **J** | ReSTIR reuse on/off: off is each GI pixel's one fresh path, denoised — the A/B for what reuse buys |
| **Q** | The reconstruction bypassed: the render resolution point-sampled |
| **P** | The final render / the raw path trace: the post (V, B, E, K), the fog (O), the light shafts (L), the SSAO |
| **U** | The soft sun shadow |

For measuring: `GALLERY_STEP_AT=n` sidesteps the camera at frame n (what it uncovers), `RESTIR_RAW=1`
shows each frame's estimate alone (no GI accumulation, no image history), `RESTIR_TEMPORAL=0`,
`RESTIR_SPATIAL=0` and `RESTIR_VISIBILITY=0` turn off one part of the reuse. `RESTIR_JITTER=0` goes back to the four window-pixel
centres instead of the TAA's Halton jitter; `GALLERY_BYPASS=1` starts with the reconstruction bypassed (Q).

For `--capture`: `RESTIR_VIEW=n`, and `RESTIR_POST=0` (the raw path trace), `RESTIR_FOG=0`, `RESTIR_SHAFTS=0`, `RESTIR_SOFT=0`, `RESTIR_SSAO=0`,
`RESTIR_REUSE=0`, `RESTIR_VALIDATE=0`, `RESTIR_ATROUS=0` each start with that part off; `RESTIR_UNTOLD=1`
hides the sun's moves from the renderer (with `GALLERY_SUN_STEP_AT`).

## Cost

The cheap configuration, RTX 3070, 1273×669 (`--bench`), Lee Chemicals as below:

| | Frame | G-buffer | Paths (sample + spatial) |
|---|---|---|---|
| Lee Chemicals, restir (its README, same size) | 4.4 | 1.7 | ~1.5 |
| Lee Chemicals, **restir-pt, cheap** | **5.4** | 1.75 | **2.25** (1.81 + 0.44) |
| SanMiguel, **restir-pt, cheap** | **3.4** | 0.76 | **1.25** (0.80 + 0.45) |

The full configuration, 1273×1370: restir measured the same way, from the commit before.
Lee Chemicals is a converted Teardown map, from above the factory (`--look 100,300,-500,450,50,150`).

| | Frame | G-buffer | Paths (sample + spatial) |
|---|---|---|---|
| SanMiguel, restir | 4.2 | 0.7 | 1.4 |
| SanMiguel, **restir-pt** | **6.7** | 0.7 | **3.9** |
| Lee Chemicals, restir | 7.7 | 2.7 | 2.9 |
| Lee Chemicals, **restir-pt** | **16.7** | 2.6 | **11.8** |
| ... reuse off (one path, no shifts) | 12.7 | 3.1 | 7.5 |
| ... validation off | 16.5 | 2.6 | 11.6 |

Where it goes: a path is six rays to restir's four, all exact; glossy pixels re-trace the paths they
borrow; and the walk's state, live across the traversal, costs occupancy — per ray the sample pass is about
twice pt-reference's (whose GI-free loop is lighter, and whose full-resolution rays are more coherent than
a sixteenth of the pixels). Slimming that state took Lee Chemicals from 19.7 to 16.7 ms. Coarse secondary
rays (restir's 64 and 16 voxels) save under a millisecond and cost the image a fifth of its indirect light,
so they are off. Halving the spatial ray budget (24 → 12) saved little: replays are not most of it.

## Motion vectors

The G-buffer writes `gMotion` (fbo 1's seventh target): for each texel, where its surface was on screen last
frame minus where it is now. A chunk's own motion comes from its pose last frame -- the host uploads
`voxel_renderer::uploadPoses(world, renderer, 99)` each frame into `prevPoses` -- and the camera's from
`prevCameraPos`/`prevCameraDir`. The resolve reprojects its history along them (the nearest surface's in
3×3 texels, with a neighbourhood clamp while anything moves) and `motion_blur` streaks along them: eight taps
over half the frame's travel (film's 180° shutter) (`restirDof.z`), at most 12 pixels at 1080 lines (`restirDof.w`). A still pixel
that showed something moving last frame (its stored history length is negative) was just uncovered, and
is clamped too, so a body crossing a still view leaves no trail. The GI's
reservoirs and accumulation still follow the camera alone: a moving body's indirect light lags a few frames.

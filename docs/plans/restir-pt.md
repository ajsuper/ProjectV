# ReSTIR PT: paths with materials, reused

Proposed 2026-10-22. Status: **delivered in part, 2026-10-22** -- `restir-pt` has replaced `restir` in the
gallery and `pt-reference` its path tracers; see the delivery log at the end for what is done, what
deviates from the design below, and what is not done.

> Measured on Teardown levels (Tuxedo Labs) converted from a local install with `mesh_voxelizer
> --td-*`. They are not in the repo and may not be redistributed, so the numbers here are a record,
> not something a checkout can rerun. Credit: `SOURCES.md`.

The gallery's `restir` renderer is ReSTIR GI: one diffuse bounce reused by reconnection, Lambertian
everywhere. Two things it cannot do:

- **Materials.** Glossiness and metallic are in every palette (`decodeMaterial`) and ignored. A glossy
  floor, a metal panel, a mirror all shade as chalk.
- **Stale light.** A reservoir caches the light its path saw (`Lo`). The renderer resets every history
  when the *sun* moves (done 2026-10-22, in `restir`, carried into `restir-pt`), but any
  other change -- an explosion, a door, a light switched on in a game -- is reused, and spread by the
  spatial feedback, until the reservoir happens to be replaced.

ReSTIR PT (Lin et al. 2022, *Generalized Resampled Importance Sampling*) answers both: it reuses whole
paths through arbitrary BSDFs, with a shift that re-traces the path's view-dependent start and reconnects
only where reconnection is safe.

## Design

### Material model -- one, shared

`include/pjv_bsdf.sc`, hoisted from the scene editor's Render-tab path tracer (`renderShaders/
path_trace.frag`), which already defines it:

| Term | Value |
|---|---|
| roughness | `alpha = max(1 - glossiness, 0.03)²` (GGX) |
| diffuse | `albedo × (1 - metallic)`, Lambert, × `(1 - F)` |
| specular | GGX D, Smith G, Schlick F; `F0 = mix(0.04 × glossiness, albedo, metallic)` |

Rule kept from the editor: glossiness 0 and metallic 0 -- every palette written before materials -- is
*exactly* Lambert (dielectric F0 is scaled by glossiness). Sampling: VNDF for the specular lobe, cosine
for the diffuse, the lobe picked by estimated weight. The editor's path tracer moves onto the include, so
the editor and the gallery cannot disagree.

### Paths

From the primary hit x1: up to **3 bounces**, BSDF-sampled, with next-event estimation to the sun at every
vertex (MIS against BSDF sampling: the sun is a disc, a glossy lobe can hit it). Emission at every vertex.
Transparent voxels stay solid (`raySceneIntersectFrom`), as now.

### Reservoir: a path, not a point

| Field | Why |
|---|---|
| seed | **random replay**: the prefix is re-traced from it at the shifted pixel |
| k | the reconnection vertex's index (0: none, replay all) |
| x_k, n_k, material of x_k | reconnect to it from a new x_{k-1} |
| ω_k (direction out of x_k), L_k (light arriving along it, the suffix's) | re-evaluate x_k's BSDF for the new incoming direction |
| target, W, M | resampling |

**Hybrid shift.** Replay the prefix x1..x_{k-1} with the stored seed; reconnect to x_k when both x_{k-1}
and x_k are rough (`alpha > 0.2`) and far enough apart (a few voxels): the path's first rough-rough pair.
A diffuse wall seen in a mirror: the mirror bounce is replayed, the wall reconnected. All-glossy paths
(mirror to mirror) are replayed whole. Jacobian: the reconnection term (as now) times the replay's
(1 for replay with the same random numbers).

**Target**: luminance of the full path contribution, primary BSDF included -- view-dependent, so the
shift re-evaluates it at the new pixel.

**MIS**: pairwise MIS (Bitterli's, as the paper uses) for spatial reuse; the temporal pair likewise.
Feedback of the spatial result into the temporal history, as `restir` does since 2026-10-22.

### Stale light: validation

Each frame a quarter of the pixels (rotating) re-trace their reservoir's path suffix from x_k. If the light
it now carries differs by more than a threshold, the cached L_k is replaced and M clamped to 1 (Kajiya's
validation). Any change of light -- told or not -- is gone from the reservoirs within four frames. The sun
reset stays (it is free and immediate).

### Denoising

Diffuse and specular separately, each demodulated (diffuse by diffuse albedo; specular by its
pre-integrated directional albedo, a small LUT). Diffuse: as now (12-frame accumulation, à-trous).
Specular: history length from roughness (a mirror keeps ~2 frames, rough ~12), reprojected at the
reflected point's virtual depth for glossy surfaces; à-trous radius scaled by roughness.

### Direct light

The sun through the BSDF, highlight included: `shadow_soft` computes diffuse + specular sun with the soft
visibility. The G-buffer carries glossiness and metallic (it is at the nine-input limit for the composite:
packed into `gDirect.a` as `glossiness × 256 + metallic` in an RGBA32F `gDirect`).

## Reference

A gallery renderer `pt-reference`: a plain accumulating path tracer on `pjv_bsdf.sc`, same sun, sky, path
length and display. It converges slowly and exactly; every milestone compares against it (linear,
downsampled, as `gi_eval.sh` did for the lag work) on a material test scene -- the test room with a glossy
floor, a metal wall and a mirror panel.

## Milestones

| # | What | Done when |
|---|---|---|
| T0 | `pjv_bsdf.sc`; the editor's path tracer on it (pixel-identical); `pt-reference`; the material test scene | the editor's Render tab unchanged; `pt-reference` converges on the test scene |
| T1 | `restir-pt` (a copy of `restir` to start): G-buffer materials; sun through the BSDF; paths with BSDF sampling + NEE; reservoirs as paths; reconnection-only shift | diffuse scenes match `restir`; glossy direct highlights match the reference |
| T2 | Hybrid shift (random replay) and pairwise MIS | glossy-floor reflections of diffuse walls converge to the reference |
| T3 | Validation | an unannounced light change settles within ~4 frames of reservoir age |
| T4 | Split, demodulated denoising; specular reprojection | reflections stable under camera motion, no ghosting |
| T5 | Retire `restir` (or keep it as the cheap option) | `restir-pt` at least as good on diffuse scenes, cost measured |

## Open questions

1. **Resolution.** `restir` traces GI for 1 pixel in 16. Glossy reflections at that density are blocky;
   1 in 4 (`--restir-gi 2` today) is likely the floor for specular. Cost to be measured in T1.
2. **Replace or sit beside.** Proposed: `restir-pt` beside `restir` until T5.
3. **Transparency** (glass refraction, rough transmission) is out of scope here; it would follow T4.

## Delivery log

### 2026-10-22 -- T0 to T3, most of T4; `restir` retired at once

Asked for: replace `restir` with ReSTIR PT (not beside it: open question 2), GI at 1/16 of the pixels first
(open question 1), and a reference path tracer that replaces the gallery's others (`tree64`, `taa`).

**Done.**

- `include/pjv_bsdf.sc`: the editor's model, expression for expression, plus a split evaluation (diffuse
  factor and specular apart) and the specular lobe's directional albedo. The editor's Render-tab path
  tracer is on it; its compiled shader is the same size to the byte with renumbered IDs (no SPIR-V
  disassembler here to diff further; not looked at on screen).
- `pt-reference` (README in its folder), and `--scene material-room`, built in main.cpp rather than
  shipped: glossy checker floor, metal wall, mirror panel, red wall, skylight, window, semi-glossy block,
  white cube, lamp. `sharedShaders/pjv_path_light.sc`: the sun as a 0.02-radian disc and the sky at 0.35,
  shared by the reference and restir-pt.
- `restir-pt`: materials in the G-buffer (`gDirect.a`, RGBA32F, as planned); the sun through the BSDF
  with the lobe widened by the disc; three-bounce BSDF-sampled paths with sun NEE and MIS; reservoirs as
  paths (seed, k, x_k and its material, Le, sun visibility, w_k, L, the reconnection's cos/d^2); the
  hybrid shift (random replay, then reconnection; k = 0 replayed whole), with invertibility checks;
  validation; diffuse and specular accumulated and filtered apart, each demodulated, the specular history
  2..12 frames by roughness and its filter held to the lobe's reach.

**Deviations, and why.**

- *w_k is sampled cosine-weighted at the reconnection vertex*, not from the BSDF. Its BSDF density depends
  on the incoming direction a shift changes; cosine makes the reflected light pi f L for any x_{k-1}, and
  lets validation re-trace the suffix without knowing where the path came from.
- *NEE at x_k is stored as visibility* and re-evaluated at the disc's centre through x_k's BSDF on a shift.
- *Weights are 1/M with M counting only shifts that succeeded*, not pairwise MIS (which needs every
  neighbour's sample shifted back, more rays). Counting failed shifts darkened reuse to 0.6-0.8 of the
  reference; this gives 0.92-1.06.
- *The specular lobe is sampled from the NDF* (the editor's), not VNDF: the editor had to stay as it was.
- *The directional albedo is Karis's analytic fit*, not a LUT.
- *Secondary rays are exact.* restir's coarse bricks (bounces past 64 voxels, their sun past 16) shrank
  every opening that was not brick-aligned and took the material room's indirect light to 0.75-0.8.
- *The à-trous luminance test is against the 3x3 mean* (the diffuse), and the specular has none: against
  the noisy centre, a sparse signal was darkened (the mirror to 0.54 of the reference without reuse).
- *Specular reprojection at the reflected point's depth is not done*; the specular history is only short.

**Measured** (material room, linear region means over pt-reference; restir-pt's README has the table):
no reuse 0.94-1.01, ReSTIR PT at 1/16 0.92-1.06, at 1/4 0.94-1.00. The mirror at 1/16 is the right
brightness but mottled; at 1/4 it reads clearly -- **open question 1: 1/4 is the floor for specular**,
as guessed; 1/16 stays the default as asked.

**Validation (T3)** is exact -- bit-identical images with and without it on an unchanged scene, once the
direction and normal a suffix starts from are rounded to the reservoir's packing before first use -- but
made no measurable difference to an untold change of light (the sun lowered 14 degrees without telling the
renderer, `RESTIR_UNTOLD=1`): SanMiguel's indirect light was 0.44 of the change behind at +64 frames and
0.03 at +256 with it or without. The lag is the image's own histories (the resolve's 64 frames, the GI's
12), which only a told change resets; reservoirs (M capped at 4) are displaced by new paths within a few
frames. So "settles within ~4 frames of reservoir age" holds for the reservoirs and is not what an untold
change waits on. Costs ~0.5 ms on Lee Chemicals.

**Cost** (RTX 3070, 1273x1370, `--bench`): SanMiguel 4.2 -> 6.7 ms a frame (paths 1.4 -> 3.9),
Lee Chemicals 7.7 -> 16.7 (2.8 -> 11.8). Six exact rays a path to restir's four coarse, replays for glossy
pixels, and occupancy: the walk's state is live across the traversal. Slimming it took Lee Chemicals from
19.7 to 16.7.

**Not done.** Pairwise MIS (T2). Virtual-depth specular reprojection (T4). Transparency (out of scope).
NEE to emissive voxels (neither renderer has it; the reference shows a lamp as fireflies for a long time).
The diffuse filter still costs the white cube a few percent against the reference.

### 2026-10-22 (later) -- made cheap; mirrors by screen-space reflection

Asked for: cheap, 1/16, coarse rays; the diffuse/specular look, not necessarily mirrors; a basic SSR for
mirror surfaces. Now the default: the paths see glossiness capped at 0.5 (every vertex rough, so every path
reconnects at x2), two bounces (pt-reference too), coarse secondary rays, random replay and validation
compiled out (`RESTIR_REPLAY`, `RESTIR_VALIDATE`). Glossiness 0.9 and up gets an SSR in the composite (last
frame's image, G-buffer march, fading back to the paths' blurred reflection). Lee Chemicals at 1273x669:
5.4 ms a frame, paths 2.25 ms (from 16.7 / 11.8 at 1273x1370 for the full configuration); restir was 4.4 /
~1.5. A mirror facing the camera mostly reflects what is off screen, which SSR cannot show.

### 2026-10-22 (later still) -- final render vs raw path trace; sun shafts

P is now the final-render switch: off is the raw path-traced image (no post, fog, shafts or SSAO). Light
shafts (`shafts.frag`, L): world-space single scattering of the sun, one coarse shadow ray per GI pixel at
a point drawn from an exponential falloff (48 finest voxels), HG g = 0.6, 16-frame reprojected history,
depth-aware 4x4 upsample. 0.04 ms on Lee Chemicals. Not done: shafts from emissive voxels.

### 2026-10-22 (evening) -- bloom, SSAO, and the disocclusion flare

Bloom (a 13-tap mip chain, 4%, N) and a two-scale Alchemy-style obscurance replacing the SSAO (0.85 on
the indirect light). Uncovered areas came in bright and faded over ~100 frames: the reuse skipped empty
reservoirs (W = 0, a path that found no light -- about half of them in the material room), which weighs
bright paths as if dark ones never happen. Spatial reuse alone was ~4x too bright; the temporal history
diluted it in steady state, but nothing without a history had one. Fixed by counting them; the reuse is
now weighted by the generalized balance heuristic (analytic shifts, so pairwise-style MIS without rays),
with a wider search for pixels with no history. A sidestep's uncovered area: 10% of pixels more than 25%
off at +1 frame before, 0.6% after; whole-frame +14% -> +1%. In the raw per-frame estimate a steady ~10-20%
excess remains (mostly the glossy floor's specular), hidden in the filtered image; not yet traced.

### 2026-10-22 (night) -- sky, fog, TAA

A new sky in pjv_path_light.sc (shared with pt-reference): deep zenith, horizon band, sunset towards the
sun, aureole, haze below the horizon (no black ground band); clouds for the camera only. Height fog
integrated exactly along the view ray (no onset kink), fading to the horizon's air colour. TAA: a 16-phase
Halton jitter over the whole render texel replaces the four window-pixel centres, so a still view
supersamples and sub-texel distant voxels stop aliasing. Not measured against pt-reference since; the sky
changed for both.

### 2026-10-22 (late) -- motion vectors, motion blur

The G-buffer writes per-texel motion (gMotion, fbo 1's seventh target) from each chunk's previous pose (the
voxel renderer's prevPoses upload, texture 99 here) and the previous camera. The resolve reprojects along it
(nearest-depth motion over 3x3, neighbourhood clamp and a history of at most 16 while anything moves), so
Voxel Mayhem's moving bodies no longer smear; a still pixel that showed something moving last frame (history
length stored negative) is clamped too, which removed the trails behind debris under a still camera. A new
motion_blur pass after the depth of field: 8 taps over half the frame's travel (a 180-degree shutter), capped at 12 px per 1080 lines, a still foreground not dragged over a moving
background; M toggles it, RESTIR_MBLUR=0 starts it off. Still open: the GI's reservoirs and accumulation
reproject through the camera only, so a moving body's indirect light lags.

Fog over dark interiors: the haze's in-scatter is scaled by how lit the air in front of a surface is (the
surface's received light against the air's brightness, rising to full over the fog's length); extinction is
unchanged. Before, a sky-coloured veil over black interiors read as medium grey once auto-exposure rose.

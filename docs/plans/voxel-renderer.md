# Voxel renderer: rebuilt for speed, then feel, then looks

The examples' game renderer (`examples/shared/voxelRenderer`, set up by `examples/shared/voxel_renderer.h`):
Voxel Mayhem, Playground, Party, Multiplayer, and the gallery (`renderer_gallery --renderer voxel`).
Explosion runs a fork (`25-explosion/nightRenderer`). Started 2026-10-20.

Priorities, in order: **1 performance, 2 a polished feel** (responsive, no ghosting or smearing on
moving things, steady when still), **3 visual complexity** (GI and the rest).

## Why a rebuild

A first pass (2026-10-20) jittered the G-buffer and accumulated samples at output resolution with long
history (weight 48, no colour clip on depth-validated history). Static views resolved far detail well;
in play it was unusable: light lagged, and moving objects -- which had no motion vectors, so their
history was refused and rebuilt from one frame -- smeared and boiled. The ray GI (one ray per 2x2 render
pixels, screen-space history) was unstable under jitter and motion. Thrown away; the measurements and
the tooling stay.

What shipped renderers do (research, 2026-10-20):
- Teardown: per-object ray march at native resolution, a velocity target (object motion included),
  stochastic lighting spatially blurred then reprojected by motion vectors, TXAA-style resolve with a
  jittered camera. AO from sparse rays in a coarse 3D occupancy volume, not screen space.
- FSR2: motion vectors and depth drive reprojection; disocclusion from reprojected depth; history
  colour-clamped (responsive); thin features "locked" against flicker; jitter cycle ~32 at 2x.
- Stable GI is world-space (probe grids, DDGI; caches per surface), not per-pixel screen-space history.

## Design

```
gbuffer (render scale)  primary ray + sun shadow ray, one march call; jittered; writes motion
probes (32x16x32)       GI probe update: an eighth of the probes a frame, 6 voxel rays each
pack                    probes filtered with their 6 neighbours, laid into one texture
ao                      AO at two scales, from the G-buffer (no bounce)
shade                   sun x shadow + probe GI x AO + emission, fog
reconstruct (output)    voxel-identity upscale + edge AA (jitter-aware)
taa (output)            motion-vector reprojection, YCoCg variance clip, fast blend
display                 grade
```

- **Motion vectors for everything.** Each chunk's pose last frame, in a renderer texture (RGBA32F, two
  texels a header slot) that `voxel_renderer::upload` fills from the Scene; the G-buffer moves the hit
  through it (`prev = prevPos + prevR * R^T * (P - pos)`) and writes the screen-space motion. TAA and any
  later history follow moving bodies instead of refusing them.
- **TAA: responsive by default.** History clipped to the current neighbourhood (variance box, YCoCg),
  Catmull-Rom history, blend ~0.1 while moving. No long accumulation.
- **Jitter through the output pixels' own centres.** At half scale a render texel covers 2x2 output
  pixels; the jitter visits their four centres in turn (diagonals first). Each frame one pixel in each
  block is sampled exactly; TAA takes 80% of a frame whose sample is the pixel's own (Gaussian on the
  sample's distance, 0.3 px) and nothing of the others while still. A still view repeats the same four
  samples, so it settles to the full-resolution, reconstruction-antialiased image and stays there; any
  change reaches every pixel within four frames. A Halton jitter with an exponential blend never
  settles: measured 0.26-0.40 flicker against 0.08 for this.
- **Disocclusion by depth.** gMotion.z is each sample's distance from last frame's camera; history keeps
  the depth it showed (its alpha). History is the same view if that depth lies in the 3x3's range of
  last-frame depths (3%); a thin feature the jitter finds only some frames stays in range. Not the same:
  tight clip, half this frame.
- **Cost first.** GI and the colour bounce are out until the base is fast and steady (the wide AO stays, as
  the stand-in for enclosure); GI comes back world-space (step R5), so motion and jitter cannot make it boil.

## Steps

| | Step | State |
|---|---|---|
| R0 | Gallery entry (`--renderer voxel`), `--bench`, `--capture`, `--look`, `--pan`, `--voxel-folder`; `frames.py` flicker measure | done |
| R1 | Strip: drop ray GI (raygi, giaccum, giblur x2) and the bounce; AO keeps both scales (the wide one stands in for GI); shade simplified; one spec loader for the HUD games | done |
| R2 | Motion vectors: previous chunk poses (`prevPoses`, `voxel_renderer::uploadPoses`), gMotion in place of gLight (sun visibility moved to gNormal.w) | done |
| R3 | TAA: motion-vector reprojection, depth disocclusion, variance clip, own-sample jitter | done |
| R4 | Speed: the G-buffer march (half of what remains), reconstruction | |
| R5 | GI, world-space: a probe grid round the camera (DDGI-like, L1 SH, no visibility maps) | done |
| R6 | The fork (`nightRenderer`), docs | |

## Measurements

Radeon 860M (RADV), on battery, window 1684x1072, render scale 0.5. GPU ms.

Before R1 (the renderer as merged, 2026-10-20, after a reboot -- earlier runs that day were with the GPU
stuck slow, ~5x, and are discarded):

| Scene | frame | gbuffer | ssao | raygi | giaccum | giblur x2 | shade | reconstruct | taa | display |
|---|---|---|---|---|---|---|---|---|---|---|
| Bistro (gallery framing) | 30.8 | 10.5 | 4.2 | 5.8 | 0.3 | 2.0 | 1.9 | 4.2 | 1.7 | 0.25 |
| Voxel Mayhem (`MAYHEM_GPU`) | 16.3 | 7.6 | 0.4 | 1.9 | 0.2 | 2.2 | 0.8 | 2.2 | 0.8 | 0.1 |

After R1 (six passes), window 840x1072 (the desktop's choice that day; compare within a batch only):

| | frame | gbuffer | ao | shade | reconstruct | taa | display |
|---|---|---|---|---|---|---|---|
| Bistro, R1 | 9.2-9.4 | 5.5 | 0.4 | 0.5 | 1.8 | 0.9 | 0.15 |
| Bistro, R3 | 9.4-9.6 | 5.6 | 0.4 | 0.4 | 1.9 | 1.1 | 0.15 |
| Voxel Mayhem, R3 | 12.7 | 8.6 | 0.4 | 0.5 | 2.0 | 0.9 | 0.2 |

Still-camera flicker (mean |frame 302 - 301|, 0-255), Bistro: no jitter 0.002; Halton 8 + exponential
blend 0.40 (wide clip and 0.03 blend: 0.26); own-sample jitter 0.08. Moving: a 3 px/frame pan and
falling debris (Mayhem, cannon through the castle wall) show no smear or ghost.

### R5: GI from a probe grid

- 32x16x32 probes, 16 finest voxels apart (2 m in Mayhem: 64x32x64 m), scrolling with the camera in whole
  cells and stored at cell mod dims (each probe's last texel names its cell; a probe whose slot holds
  another cell starts over). Shading blends the 8 round the point (offset half a cell along the normal),
  weighted toward those on the normal's side and by how usable each is (the share of its rays that did
  not start inside solid); the sky ambient beyond the grid, faded over its last 3 cells.
- An update: 6 rays on a turned spherical Fibonacci lattice; a hit's light is the sun on it (shadow ray),
  its emission, and the nearest probe's light there (a bounce more each update). Hysteresis 0.9, 0.5
  when the light plainly changed. Probe texture 1024x16 (4 targets), packed 4096x16.
- Cost lessons: updating a scattered eighth cost as much as updating all (every GPU wave held a few
  tracing probes): 2.6 ms; by 8x8 texture block: 1.37 ms (Mayhem), 0.7 ms (Bistro). Twelve rays a
  quarter of the probes, nearest-8 lookups at the hits, 24-spacing rays: 5.1 ms.
- Noise: 6 rays an update left 2 m blotches; the pack filters each probe with its 6 neighbours (history
  stays unfiltered, or it would blur again every frame).
- Bug found: GLSL integer % is undefined for negatives; cells below zero landed in the wrong slots. Floor
  mod through floats.

| Bistro, 840x1072 | frame | gbuffer | probes | ao | shade | reconstruct | taa |
|---|---|---|---|---|---|---|---|
| R3 (no GI) | 8.5-9.5 | 4.5-5.6 | - | 0.4 | 0.4 | 1.8-2.3 | 0.9-1.1 |
| R5 | 10.3 | 5.5 | 0.7 | 0.4 | 0.5 | 1.9 | 1.0 |

Still-camera flicker, Bistro: 0.17 (0.08 without GI: the probes keep refining).

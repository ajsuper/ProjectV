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
ao                      AO at two scales, from the G-buffer (no bounce)
shade                   sun x shadow + sky ambient x AO + emission, fog
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
| R5 | GI, world-space | |
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

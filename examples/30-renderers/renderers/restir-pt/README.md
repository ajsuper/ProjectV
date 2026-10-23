# restir — ReSTIR GI at a sixteenth of the pixels, upscaled

`--renderer restir` (`--restir-gi 2`: GI at a quarter of the window's pixels instead)

Two bounces of light, like the path tracers here, but traced for a sixteenth of the window's pixels and
then **shared**: each pixel's light sample is resampled against last frame's and its neighbours' (ReSTIR
GI), so one ray does the work of many. What noise is left goes through an SVGF-style denoiser. The camera
rays are traced at half the window's resolution each way and rebuilt to full by the voxel renderer's
reconstruction, then accumulated over frames.

It started in the ray lab (the fork at `../ProjectV-raylab`, modes 12 and 13).

## The passes

| # | Pass | Resolution | What it does |
|---|---|---|---|
| 1 | `gbuffer` | ½ × ½ | Camera ray and one sun shadow ray (straight at the sun's centre: no noise), handing on the sun's visibility and its blocker's distance; the hit face and chunk rotation for the reconstruction |
| 2 | `sample` | ¼ × ¼ | One candidate per GI pixel: a cosine bounce to x2, and x2's outgoing light (emission, sun, one more bounce with its sun) |
| 3 | `temporal` | ¼ × ¼ | ReSTIR temporal reuse: the candidate against last frame's reservoir for the same surface -- the one the spatial pass ended last frame with (M capped at 4) |
| 4 | `spatial` | ¼ × ¼ | ReSTIR spatial reuse: five neighbours' reservoirs, then a visibility ray for a borrowed sample; resolves to indirect light divided by albedo, and hands its reservoir on as next frame's temporal history |
| 5 | `gi_accumulate` | ¼ × ¼ | Temporal accumulation of that light (12 frames at most), with luminance moments (variance) |
| 6–9 | `atrous1/2/4/8` | ¼ × ¼ | À-trous filter at steps 1, 2, 4 and 8 (61 GI pixels across), edge-stopped by normal, plane and a loose (6σ) luminance test: the GI is meant to be soft |
| 10 | `ssao` | ½ × ½ | Subtle screen-space AO: twelve samples, six voxels round, in a fixed 4×4 rotation pattern |
| 10b | `shadow_soft` | ½ × ½ | The sun's light with a soft shadow: a penumbra as wide as the blocker's distance × the sun's width, found and filtered in screen space on the same surface (PCSS-style) |
| 11 | `composite` | ½ × ½ | Edge-aware upsample of the indirect light; the direct light and albedo × indirect, kept apart |
| 12 | `ao_apply` | ½ × ½ | The SSAO blurred over its 4×4 pattern (depth-aware), darkening the indirect light by up to half; the sun's light added; the distance fog; the views (G) |
| 13 | `reconstruct` | window | The voxel renderer's reconstruction (`examples/shared/voxelRenderer/shaders/reconstruct.frag`, copied): voxel by voxel, antialiased |
| 14 | `resolve` | window | Temporal accumulation: the 2×2 jitter's four samples, and what noise is left |
| 15 | `meter` | 64 × 64 | Log luminance of the frame, 16 samples a texel, for the auto-exposure |
| 16 | `exposure` | 1 × 1 | The auto-exposure (centre-weighted average, eased from last frame's) and the depth of field's autofocus (the centre's distance, eased) |
| 16b | `dof` | window | Depth of field round the focus: a scatter-as-gather disc, at most 1.5 px (at 1080 lines) for the far distance |
| 17 | `display` | window | The post (vignette, exposure, grade; P) and the engine's neutral display transform (`pjvDisplay`) |

Each render texel is traced through the centre of another of the 2×2 window pixels it covers each frame,
as the voxel renderer does; a still view repeats the same four and the resolve settles on them. A GI
pixel takes its visible point from one render texel of its 2×2 block, a different one each frame.

Every ray goes through the engine's `raySceneIntersectFrom` (the bare nearest-solid query, as the voxel
renderer uses), from one call site per pass. So transparent materials draw as solid here.

**Coarse secondary rays** (`restir_trace.sc`, as the ray lab's mode 10): a bounce ray counts a non-empty
4³ brick as solid past 64 voxels from its origin, and the sun's shadow ray from a bounce's surface past
16. Its light is seen only indirectly, so the bricks do not show. The camera ray and its sun shadow are
exact.

## The look

- **The sky is dimmed** (`RESTIR_SKY_LIGHT`, 0.35 of the gallery's shared sky). That sky's zenith is about
  four times the voxel renderer's, and lighting by it flooded every surface with blue-white.
- **Colour** follows the engine's convention (`include/pjv_color.sc`, `docs/plans/color-convention.md`):
  the palette arrives decoded to linear, and the display is the neutral transform -- identity up to 0.76,
  a highlight shoulder above, the exact sRGB curve, a dither.
- **The post** (`display.frag`, on by default; P turns it all off), each part in linear light before the
  tone curve, each the identity when off:

  | Key | Part | What |
  |---|---|---|
  | V | vignette | 15% darker in the corners, flat over the middle |
  | E | auto-exposure | passes 15-16: the frame's log luminance metered at 64x64 (centre-weighted), the exposure that brings it 80% of the way to 0.08 (a sunlit exterior needs about none), held to -2..+3 stops and adapted over time (darkening faster than brightening) |
  | B | grade | contrast ×1.08 as a power about middle grey (no black crush), saturation ×1.10, a slightly warm white |
  | K | depth of field | autofocus on the view's centre (eased, as a camera racks); the far distance blurs 1.5 px at 1080 lines, nearer than the focus as much |

- **The scene's own**, outside the post:

  | Key | Part | What |
  |---|---|---|
  | U | soft sun shadow | the sun as a disc 0.04 rad across (~4× the real one): the penumbra is 4% of the blocker's distance -- a 10 m wall's shadow-edge 40 cm wide at its foot |
  | O | distance fog | towards the horizon sky's colour in that direction, warmed towards the sun; its length 1.6 scene radii (held to 300..3000 units), starting a tenth of the way out, at most 85% |

  The views (G) skip the post and the fog. For `--capture`: `RESTIR_POST=0`, `RESTIR_FOG=0`, `RESTIR_SOFT=0`
  start with each off, `RESTIR_VIEW=n` on view n.

## Stable at a distance

The camera rays are jittered, and where a pixel spans several voxels each frame's ray lands on a different
one. Every test that asks "is this the same surface as last frame?" therefore scales with the pixel's
width at that distance:

- the GI history, the reservoir reuse and the upsample (`restirSameSurface`) widen their plane test by
  the GI pixel's width, and stop comparing normals once that width passes a voxel;
- the denoiser's edge-stopping does the same;
- the resolve keeps a still camera's history (64 frames) unconditionally, and while the camera moves clamps it to
  this frame's 3×3 neighbourhood instead of rejecting it surface by surface.

Before, all of them demanded the exact same face and the distance shimmered: on Lee Chemicals, 23% of
pixels changed by more than 8/255 between still frames; after, none did (mean change 9.0 → 0.04).

## The reuse, in brief

A **reservoir** holds one light sample — a point x2 on some surface, its normal and the light leaving it
toward the pixel — plus weights. Resampling picks between reservoirs in proportion to how much light each
sample would bring *here* (the target function: its luminance times the cosine at this pixel). Taking a
sample formed at another pixel's visible point means reconnecting to x2 from a different place, so its
weight is corrected by the **Jacobian** of that shift (`restirJacobian` in `restir_common.sc`). Shifts
that change it more than 8× either way are refused: those are the near-grazing reconnections whose
weights blow up.

A borrowed sample is checked for visibility before it is used. Without that, light leaks round
corners; with the textbook fix (zeroing it), the image darkens where small lights sit behind corners,
so a blocked one falls back to the pixel's own reservoir instead.

## Histories, and how fast the light settles

The reuse is in the **reservoirs** -- paths, re-weighted for each pixel and frame -- so the radiance
histories on top are short. The spatial pass's reservoirs are the next frame's temporal history (as ReSTIR
DI does), which spreads a rare good path a neighbourhood further each frame.

| History | Length | Starts over when |
|---|---|---|
| reservoirs (`temporal`) | M ≤ 4 | the sun moves: a reservoir's cached light (`Lo`) is the old sun's |
| GI light (`gi_accumulate`) | 12 frames | the sun moves, or the surface is new (reprojection) |
| image (`resolve`) | 64 still, 16 moving | the sun moves; while moving, clamped to the 3×3 neighbourhood |

Measured on the test room's window view, the sun lowered 14° (indirect light only, fraction of the
change not yet shown): before, 0.99 / 0.85 / 0.42 at +2 / +16 / +64 frames -- the 32-frame GI history,
the 1024-frame image history and the reservoirs all dragged the old light along; after, 0.78 / 0.42 /
0.16, which is the GI settling from scratch. Flicker on a still view (low frequency, linear): 0.11% of
the light before, 0.34% after.

Not yet handled: a change of light the renderer is not told of (anything but the sun, in a game). A
reservoir's `Lo` never expires, and the feedback passes it on; the fix is **validation** -- re-tracing
a share of the reservoirs' paths each frame and dropping those whose light changed -- planned with
ReSTIR PT (`docs/plans/restir-pt.md`).

## Keys

| Key | |
|---|---|
| **G** | The image / indirect light only / direct light only / ambient occlusion only |
| **J** | ReSTIR reuse on/off: off is each GI pixel's one fresh sample, denoised — the A/B for what reuse buys |
| **Q** | The reconstruction bypassed: the render resolution point-sampled |
| **C** | Palette colours decoded from sRGB / read as linear |

## Cost

GPU ms per frame, RTX 3070, 1273×669 (`--bench`). Lee Chemicals is a converted Teardown map (7,479
chunks, 213 bodies, ~3 GB on the GPU) at scale 10, from above the factory
(`--look 100,300,-500,450,50,150`).

| | Frame | G-buffer | GI (sample + reuse + denoise) |
|---|---|---|---|
| Full-resolution G-buffer, GI at ¼ of the pixels | 10.4 | 4.9 | 5.0 |
| ... GI at 1/16 of the pixels | 7.1 | 4.8 | 1.8 |
| **½ × ½ G-buffer + reconstruction (now)** | **4.4** | **1.7** | **1.8** |

The reconstruction is 0.4 ms, SSAO and its blur under 0.1. (taa, the one-sample path tracer, took 15.7 ms
on this view.)

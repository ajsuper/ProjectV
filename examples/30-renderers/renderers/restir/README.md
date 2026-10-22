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
| 1 | `gbuffer` | ½ × ½ | Camera ray and one sun shadow ray (straight at the sun's centre: hard shadows, no noise); the hit face and chunk rotation for the reconstruction |
| 2 | `sample` | ¼ × ¼ | One candidate per GI pixel: a cosine bounce to x2, and x2's outgoing light (emission, sun, one more bounce with its sun) |
| 3 | `temporal` | ¼ × ¼ | ReSTIR temporal reuse: the candidate against last frame's reservoir for the same surface |
| 4 | `spatial` | ¼ × ¼ | ReSTIR spatial reuse: five neighbours' reservoirs, then a visibility ray for a borrowed sample; resolves to indirect light divided by albedo |
| 5 | `gi_accumulate` | ¼ × ¼ | Temporal accumulation of that light, with luminance moments (variance) |
| 6–9 | `atrous1/2/4/8` | ¼ × ¼ | À-trous filter at steps 1, 2, 4 and 8 (61 GI pixels across), edge-stopped by normal, plane and a loose (6σ) luminance test: the GI is meant to be soft |
| 10 | `ssao` | ½ × ½ | Subtle screen-space AO: twelve samples, six voxels round, in a fixed 4×4 rotation pattern |
| 11 | `composite` | ½ × ½ | Edge-aware upsample of the indirect light; the direct light and albedo × indirect, kept apart |
| 12 | `ao_apply` | ½ × ½ | The SSAO blurred over its 4×4 pattern (depth-aware), darkening the indirect light by up to half; the views (G) |
| 13 | `reconstruct` | window | The voxel renderer's reconstruction (`examples/shared/voxelRenderer/shaders/reconstruct.frag`, copied): voxel by voxel, antialiased |
| 14 | `resolve` | window | Temporal accumulation: the 2×2 jitter's four samples, and what noise is left |
| 15 | `display` | window | ACES, gamma, and the voxel renderer's grading: contrast ×1.12, saturation ×1.25, a dither |

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
- **The palette is decoded from sRGB** (C toggles it). The Teardown converter writes colours as sRGB bytes;
  the engine's renderers all read them as linear, which lifts and greys every colour. Off, restir reads them
  as the others do.
- **The voxel renderer's grading** after the tone map.

## Stable at a distance

The camera rays are jittered, and where a pixel spans several voxels each frame's ray lands on a different
one. Every test that asks "is this the same surface as last frame?" therefore scales with the pixel's
width at that distance:

- the GI history, the reservoir reuse and the upsample (`restirSameSurface`) widen their plane test by
  the GI pixel's width, and stop comparing normals once that width passes a voxel;
- the denoiser's edge-stopping does the same;
- the resolve keeps a still camera's history unconditionally, and while the camera moves clamps it to
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

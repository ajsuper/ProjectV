# reprojection — temporally reprojected path tracer

`--renderer reprojection`

A from-scratch unidirectional path tracer with **temporal reprojection**, so the image stays converged *even while the camera moves* (no hard reset). Each frame path traces a single sample per pixel and reuses history by projecting this frame's world-space hits back through the previous frame's camera. Four passes (`render.json`):

1. **path_trace** — 1-spp path trace (cosine-weighted diffuse GI + next-event estimation to the sun with shadow rays), writing a G-buffer of radiance, world position, normal and albedo. The primary ray is deterministic (no jitter) so each pixel locks to one surface point for exact history reuse.
2. **reproject_accumulate** — reprojects each pixel into the previous frame, validates the match against the stored position/normal G-buffer (rejecting disocclusions), and folds the new sample into a ping-pong history buffer with a *motion-adaptive* running average. History is point-sampled at the nearest texel (no per-frame bilinear smearing). When the camera is still the average grows very long so noise converges toward zero; when it moves the history is kept short so it stays responsive and does not ghost/smear.
3. **denoise** — an edge-aware spatial à-trous/bilateral filter guided by the noise-free G-buffer (normal + world-position + albedo edge-stopping). This cleans the residual noise that the deliberately-light moving history leaves, so motion is both sharp *and* clean without leaning on heavy temporal blending.
4. **display** — ACES tone-map + gamma to the screen

The reprojection maths (`worldToUV` in `reproject_accumulate.frag`) is the exact inverse of the engine's `rayStartDirection`, and the history buffer is a ping-pong framebuffer (a pass that both reads and writes framebuffer 2), which the engine detects automatically. All renderers share the engine-level voxel DDA traversal in `pjv_utils_DDA.sc`.

A blue-noise LUT (`LDR_RGBA_7.png`) is uploaded to the GPU at startup to decorrelate samples between frames.

## When to reach for it

When the camera moves and the image still has to hold up. It is the answer to
[tree64](../tree64/)'s hard reset: instead of throwing history away, it projects this frame's
world-space hits back through the previous frame's camera and keeps what still matches.

The cost is machinery — a G-buffer, a validation step that rejects disocclusions, and a
motion-adaptive history length. That machinery is reused by [fast](../fast/) and every cascade
renderer here, so this is the one to read to understand the rest.

## Files

```
render.json       Four passes: path_trace, reproject_accumulate, denoise, display
resources.json    Shaders, render targets, framebuffers (framebuffer 2 is ping-pong)
shaders/          The four fragment shaders plus the fullscreen-quad vertex shader
```

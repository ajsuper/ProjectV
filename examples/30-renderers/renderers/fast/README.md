# fast — direct sun and screen-space AO, at half resolution, with TAA

`--renderer fast` (or pick it in the gallery's panel)

The cheapest renderer here, for a reliable image at a predictable cost rather than full global
illumination. No random GI bounces: every render texel is shaded from one camera ray and one hard sun
shadow ray, and the ambient is the sky through screen-space AO. It traces a quarter of the window's
pixels and rebuilds the window from what they saw. Plan: `docs/plans/fast-renderer.md`.

## The passes

| # | Pass | Resolution | What it does |
|---|---|---|---|
| 1 | `gbuffer_shade` | ½ × ½ | One camera ray (jittered over 16 phases inside the texel, for the TAA) and one hard sun shadow ray, through one march call site. Writes the hit, its normal, albedo and direct sun light, and what the reconstruction needs to know the whole voxel: its face's centre and edge, and its chunk's rotation. Motion vectors by the camera alone |
| 2 | `ssao` | ½ × ½ | Screen-space AO: 12 samples in the hemisphere, each projected into the G-buffer; the sky's colour along each unoccluded one. A blue-noise rotation per pixel and per frame |
| 3 | `combine_blur` | ½ × ½ | A 5×5 bilateral blur of the AO, stopped at normal and plane changes; then direct + albedo × sky light |
| 4 | `reconstruct` | window | The voxel renderer's reconstruction (copied from restir-pt): each window pixel tests the voxels its 3×3 render texels saw against its own ray, antialiased by coverage |
| 5 | `resolve` | window | TAA: 64 frames still, 16 moving, reprojected by the motion vectors and clamped to the 3×3 neighbourhood |
| 6 | `display` | window | The engine's display transform |

At 1920×1080 (`GALLERY_SIZE=1920x1080 --bench`): **2.79 ms** — `gbuffer_shade` 1.20, `ssao` 0.21,
`combine_blur` 0.20, `reconstruct` 0.90, `resolve` 0.12. It was 4.55 ms when it traced every window
pixel.

## Settings

In the gallery's panel: the AO's radius, intensity and power (`fastSettings`, xyz), its blur (w); TAA
on or off (off: no jitter, each frame alone); the reconstruction on or off (off: the half-resolution
frame point-sampled).

## What it gets wrong, deliberately

- **The AO sees only what the camera sees.** An occluder off-screen or behind a nearer silhouette
  contributes nothing, so contact shadows soften at the frame's edges.
- **Moving chunks ghost.** Its motion vectors are the camera's alone; restir-pt also follows each
  chunk's motion. The gallery's scenes do not move.

## When to reach for it

When you want a reliable image at a predictable cost rather than a correct one: nothing to converge
except the jitter, so a still view is clean within a few frames. It is also the renderer
[10-scene-previewer](../../../10-scene-previewer/) was cut down from.

## Files

```
render.json       Six passes: gbuffer_shade, ssao, combine_blur, reconstruct, resolve, display
resources.json    Shaders, render targets (the G-buffer at half scale), framebuffers
shaders/          The six fragment shaders
```

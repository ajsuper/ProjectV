# world-cascade — world-space radiance cascades

`--renderer world-cascade`

The same cascade structure as [radiance-cascade](../radiance-cascade/), with the gather moved out
of screen space: every cascade texel casts a **real voxel-DDA ray** into the scene. That single
change fixes everything screen space gets wrong, and costs what tracing rays costs.

## What changes

| | screen-space | world-space |
|---|---|---|
| Gather | marches the depth buffer | traces the voxel DDA |
| Off-screen occluders | invisible | seen |
| Sky visibility | wrong wherever the sky is occluded on screen | correct |
| Camera pan | changes the lighting | does not |

Two further things make it stable rather than merely correct:

- **Probes snap to a world grid.** A probe anchored to a pixel moves with the camera, so its
  gathered radiance changes every frame and the temporal filter never converges. Snapping sample
  positions to a fixed world-space lattice means the same probe is the same probe from frame to
  frame, which is what makes accumulation work at all.
- **Probe interpolation is geometry-aware.** Interpolating between neighbouring probes across a
  wall leaks light through it. Weights are gated on depth and normal, so a probe on the far side of
  a surface does not contribute to it.

## Passes

`render.json`, ten passes:

| # | Pass | What it does |
|---|------|--------------|
| 1 | `gbuffer` | Primary ray: position, normal, albedo, direct sun. |
| 2–5 | `cascade3` → `cascade0` | The hierarchy, coarsest first, each merging with the level above. The gather traces the DDA — see `sharedShaders/pjv_cascade_ws.sc`. |
| 6 | `resolve` | Cascade atlas to one indirect irradiance value per pixel, cosine-weighted, geometry-aware interpolation. |
| 7 | `accumulate` | Temporal mean of the indirect term alone. Ping-pong; the engine detects it. |
| 8 | `compose` | `direct + albedo * indirect`. |
| 9 | `taa` | Resolves the primary ray's sub-pixel jitter into anti-aliased edges. |
| 10 | `display` | ACES tone-map and gamma. |

Note that only the *indirect* term is temporally filtered. Direct sun comes from the G-buffer and
stays crisp — filtering it would soften shadow edges for no benefit, since it is not noisy.

## When to reach for it

This is the best-looking renderer in the gallery, and it is the one
[40-advanced-renderer](../../../40-advanced-renderer/) grew out of. If you want to see where the
engine's rendering is actually headed, read that example instead — it replaced these four cascades
with a world-space probe atlas anchored to voxel faces, and added animation, transparency and
refraction on top.

[world-face-cascade](../world-face-cascade/) is the intermediate step between the two.

## Files

```
render.json       Ten passes
resources.json    17 render targets, 9 framebuffers
shaders/          Nine fragment shaders plus the fullscreen-quad vertex shader
```

# radiance-cascade — screen-space radiance cascades

`--renderer radiance-cascade`

Radiance cascades computed in **screen space**: the gather samples the G-buffer rather than tracing
rays into the scene. The cheapest of the three cascade renderers here, and the one that shows why
the other two exist.

## The idea

A radiance cascade is a hierarchy of probes trading angular resolution against spatial resolution.
Near the shading point you want many probes with few directions each; far away you want few probes
with many directions, because distant radiance changes slowly across space but not across angle.
Four cascades, merged coarse-to-fine, approximate a full directional gather for far less work than
casting that many rays per pixel.

This variant gathers by marching the **depth buffer** — the standard screen-space approach, and the
reason it is fast.

## Why it is here

It is kept for the comparison, not because it is the one to use. Screen-space gathering can only
see what the camera can see, so:

- Light from geometry behind the camera, or off the edge of the frame, simply is not gathered.
- Sky visibility is wrong wherever the sky is occluded on screen but open in the world, which on a
  scene like Sponza is most of it.
- Panning the camera changes the lighting, because it changes what the gather can find.

Those are not tuning problems. They are what screen space *is*, and seeing them next to
[world-cascade](../world-cascade/) — same cascade structure, same merge, gather traces real voxel
rays instead — is the clearest statement this gallery makes about the trade.

## Passes

`render.json`, eight passes:

| # | Pass | What it does |
|---|------|--------------|
| 1 | `gbuffer` | Primary ray: position, normal, albedo, direct sun. |
| 2–5 | `cascade3` → `cascade0` | The cascade hierarchy, coarsest first. Each merges its own gathered interval with the already-merged cascade above it (`upperCascade`). The shared maths is in `sharedShaders/pjv_cascade.sc`. |
| 6 | `resolve` | Cascade atlas to one indirect irradiance value per pixel. |
| 7 | `accumulate` | Temporal mean of the indirect term. |
| 8 | `display` | ACES tone-map and gamma. |

Note the merge order: cascades run coarse to fine, so `cascade0` — the finest — is last and has
every coarser level already folded into its input.

## Files

```
render.json       Eight passes
resources.json    14 render targets, 7 framebuffers
shaders/          Seven fragment shaders plus the fullscreen-quad vertex shader
```

Shared cascade maths lives in `../../sharedShaders/pjv_cascade.sc` and
`pjv_cascade_common.sc`, so all three cascade renderers stay in step.

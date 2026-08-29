# face — per-face flat global illumination

`--renderer face`

Lights every **voxel face** exactly once, as a single flat colour, rather than lighting every
pixel. Full GI plus direct sun, with no spatial noise at all — because there is nothing to be
noisy *across*: every pixel covering the same face reads the same value.

## The idea

The other renderers here shade per pixel and then spend most of their machinery removing the noise
that produces. This one changes the unit of work instead.

A voxel face is uniquely identified by the voxel it belongs to and which of its six sides it is,
and that identity is exact — no floating-point tolerance, no reprojection guess. So the renderer
derives it once per pixel, lights the *face* from its centre point, and every pixel on that face
gets the identical result.

That buys two things the reprojecting renderers work hard for:

- **Zero spatial noise**, with nothing to blur. There is no denoiser here because there is nothing
  to denoise; a face is one colour by construction.
- **A history gate that cannot ghost.** Temporal accumulation accepts the previous frame's value
  only when the face key matches exactly. A disocclusion is not a near-miss to be validated against
  depth and normal thresholds — it is a different integer, and it is rejected outright.

The cost is that this is the *definition* of a blocky look. Lighting cannot vary across a face, so
there is no gradient down a wall and no soft shadow terminator within one voxel. That is a style
decision as much as a performance one, and it is the reason this renderer exists alongside the
others rather than replacing them.

## Passes

`render.json`, four passes:

| # | Pass | What it does |
|---|------|--------------|
| 1 | `gbuffer_face` | One primary ray per pixel. Shades nothing: it identifies which voxel face was hit and emits a stable identity plus the face centre. The DDA marches at LOD 0 so `foundBox` is always a single finest-resolution voxel, which is what makes the integer voxel index exact. |
| 2 | `face_light` | Lights each face once, from its centre: direct sun plus a full GI gather. |
| 3 | `face_accum` | Temporal mean, gated on exact face-key equality. Point-samples the nearest history texel; a key mismatch discards rather than blends. |
| 4 | `display` | ACES tone-map and gamma. |

## When to reach for it

When the blocky aesthetic is what you want, or when ghosting is the thing you cannot tolerate.
It is also the cleanest demonstration in this repository of choosing a better unit of work instead
of a better filter.

[world-face-cascade](../world-face-cascade/) takes this renderer's face identity and gives it a
world-space cascade gather, which is the combination the gallery ends on.

## Files

```
render.json       Four passes: gbuffer_face, face_light, face_accum, display
resources.json    8 render targets, 3 framebuffers
shaders/          The four fragment shaders plus the fullscreen-quad vertex shader
```

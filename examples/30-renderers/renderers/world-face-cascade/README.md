# world-face-cascade — world-space cascades, anchored per face

`--renderer world-face-cascade`

[world-cascade](../world-cascade/)'s gather combined with [face](../face/)'s per-face identity. The
cheapest of the world-space cascade renderers, and the last stop before
[40-advanced-renderer](../../../40-advanced-renderer/).

## The idea

world-cascade anchors its probes to a snapped world grid. This one anchors them to **voxel face
centres** instead, which changes two things at once:

- **Far fewer gather rays.** Every screen pixel covering a given face shares one probe origin, so
  the effective probe count drops to roughly one per visible voxel face. Probe spacing is coarsened
  to match (`PROBE_SPACING0 16` against the world variant's default), and gather cost falls with
  it. This is the speed win.
- **A history gate that cannot ghost.** The resolved indirect irradiance is accumulated by exact
  voxel-face key — `face`'s trick — rather than by reprojecting and validating against depth and
  normal thresholds. A disocclusion is a different integer, not a near-miss.

A TAA pass then anti-aliases the composite, because the per-face indirect term is flat across a
face but the primary ray is still jittered per pixel.

## The trade

Indirect light is constant across each voxel face. On this geometry that is often invisible —
faces are small and the indirect term varies slowly — but it is a real limit, and it is the same
one [face](../face/) accepts deliberately. Where it shows is a large flat surface with a strong
lighting gradient across it, which will step rather than ramp.

## Passes

Structurally identical to [world-cascade](../world-cascade/) — ten passes, same names — with the
cascade gather anchored to face centres and the accumulation keyed on face identity. The G-buffer
carries a sixth target for the face key, which is why the upper-cascade sampler binds at register 6
here against world-cascade's 4.

## Files

```
render.json       Ten passes
resources.json    17 render targets, 9 framebuffers
shaders/          Nine fragment shaders plus the fullscreen-quad vertex shader
```

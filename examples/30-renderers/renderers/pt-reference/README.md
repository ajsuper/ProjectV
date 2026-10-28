# pt-reference — the reference path tracer

`--renderer pt-reference` (the gallery's default)

One path per pixel per frame, nothing reused, nothing filtered, averaged for as long as the view stays
still. It is slow and it is exact, and that is its job: it is what the other renderers are measured
against — `restir-pt` above all, which renders by the same light, through the same materials, along paths
of the same length, and shows them through the same display transform.

It replaced the gallery's two older path tracers, `tree64` (accumulation, Lambertian only, its own sun)
and `taa` (a temporal-reprojection demo on a one-sample path trace). Neither knew about materials, and a
reference that disagrees with the renderer it checks about what a surface is cannot check it.

## The passes

| # | Pass | What it does |
|---|---|---|
| 1 | `trace` | The camera ray, jittered inside the pixel; then up to two bounces, each direction sampled from the surface's BSDF, with next-event estimation to the sun at every vertex. Both strategies can find the sun — a glossy lobe often does — so each is weighted against the other by the power heuristic. Emission is added wherever a path meets it. |
| 2 | `accumulate` | The exact running mean of every path since the camera or the sun last moved (`1 / n`, not an exponential average, which would never settle). |
| 3 | `display` | The engine's neutral display transform (`pjvDisplay`) at exposure 1, no look on top. |

## What it shares, and with whom

- **The material model** is the engine's `shaders/include/pjv_bsdf.sc`: GGX over Lambert, from the palette's
  glossiness and metallic. The scene editor's Render tab and `restir-pt` shade through the same file.
  Glossiness 0 and metallic 0 is exactly Lambertian, so every scene written before materials looks as it
  always did.
- **The light** is `sharedShaders/pjv_path_light.sc`: the gallery's sun and sky rig, with the sky at 0.35
  (as restir lit by) and the sun a disc 0.02 radians across each way whose radiance integrates to the rig's
  irradiance. pjv_sun_sky.sc's own 0.3-radian disc is a display convenience; a glossy floor would mirror it
  as a blob a sixth of the sky across.
- **The path length** is two bounces (`PATH_BOUNCES` in main.cpp), as restir-pt's.

Every ray is exact (restir-pt's secondary rays are coarse, and its paths cap glossiness: compare it on
rough surfaces) and stops at the first solid voxel (`raySceneIntersectFrom`): transparent materials draw
as solid, as in restir-pt. There is no next-event estimation to emissive voxels — a small bright emitter is
found only by chance and shows as fireflies for a long time — and no firefly clamp: a clamp is a bias, and
this renderer must not have one.

## Comparing with it

```bash
cd build/examples/renderer_gallery
GALLERY_CAPTURE_AT=600 ./renderer_gallery --renderer pt-reference --scene material-room --capture ref
GALLERY_CAPTURE_AT=300 RESTIR_POST=0 RESTIR_FOG=0 RESTIR_SSAO=0 \
    ./renderer_gallery --renderer restir-pt --scene material-room --capture rpt
```

`--scene material-room` is a room built in main.cpp for exactly this: a checkered glossy floor, a metal
wall, a mirror panel, a red wall, a window and a skylight the sun falls through, a semi-glossy block, a
white cube and a small lamp. restir-pt with its post, fog and SSAO off shows the same quantity this does.
Compare in linear light, over regions or downsampled — 600 frames still leave visible noise indoors.

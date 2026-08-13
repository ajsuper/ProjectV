# AdvancedRenderer

A real-time **world-space radiance-cascade** global illumination renderer for ProjectV voxel scenes. One renderer, ten passes, no menu — it loads a Compose scene from disk, frames it, and lets you fly around it under a sun you can move.

It began as the `worldCascadeRenderer` of the [PathTracer](../PathTracer/) example, which exists to compare six approaches side by side. This one exists to be the good approach, brought up to the engine as it is today. See [What changed from PathTracer](#what-changed-from-pathtracer) for the specifics.

## The pipeline

`advancedRenderer/render.json` is the pass order; `advancedRenderer/resources.json` names the shaders and the targets they write.

| # | Pass | Writes | What it does |
|---|------|--------|--------------|
| 1 | `gbuffer` | position, normal, albedo, direct, **face, key** | One primary ray per pixel. Also the **crisp half of the image** — soft-shadowed direct sun plus voxel emission — kept out of every temporal filter downstream. |
| 2–5 | `cascade3` → `cascade0` | one radiance atlas each | The radiance cascades, coarsest first. Each texel casts **one real voxel ray** over its cascade's interval and merges what it did not hit with the cascade above. Probes are anchored to **voxel face centres**, and the atlases are a quarter of the screen on each axis. |
| 6 | `resolve` | indirect | Cascade 0 → one indirect irradiance value per pixel, cosine-weighted over the probe's directions and bilaterally interpolated between probes. |
| 7 | `accumulate` | indirect + history | Temporal mean of that indirect term **alone**, gated on **exact voxel-face identity** with a geometric fallback. Ping-pong; the engine detects it. |
| 8 | `compose` | HDR | `direct + albedo * indirect`, where the indirect is **flat across each voxel face**, plus a per-pixel rough specular. |
| 9 | `taa` | HDR + history | Resolves the primary ray's sub-pixel jitter into anti-aliased edges. Catmull-Rom history fetch so motion stays sharp. Ping-pong. |
| 10 | `display` | the window | Exposure, ACES, gamma, then contrast and saturation. All the colour grading lives here. |

### Why the light is separated from the albedo

Lighting is low frequency and albedo is not. A voxel boundary is a step in albedo, but the light arriving either side of it is nearly the same. Blurring a lit image blurs the voxel colours with it; blurring the light on its own and multiplying a sharp albedo back in afterwards does not. That is why `resolve` writes irradiance rather than colour, `accumulate` averages only that, and `compose` re-applies the albedo at the end.

The same split is why direct sunlight never enters the temporal accumulation: a shadow edge is high frequency too, and it is already noise-free.

### Why the lighting is per voxel face

Indirect light on a voxel face is *one number*. The screen-space probe grid this renderer started
with did not know that: it sampled a signal whose natural rate is per-surface-element on a grid
spaced in pixels, so the density was set by how far away you happened to be standing rather than by
the geometry. Embracing the real rate changes three things:

- **The gather origin is the voxel face centre** (`gFace`), so every screen probe landing on a face
  traces from one camera-independent origin. Probe spacing goes from 8 px to 16 — a quarter the
  probes and a quarter the gather rays — and the result is steadier rather than coarser, because
  the origins stop drifting as the camera moves. It also replaces the world-grid snap that used to
  approximate this; the voxelisation already *is* that grid.
- **Temporal history is gated on exact face identity** (`gKey`), built from `hit.voxelCoord` — the
  integer the DDA carried through the march. Two pixels on the same face share a converged value
  whatever either did last frame.
- **A face's value only ever comes from a probe standing on that face.** A probe's gathered radiance
  is camera-independent: face-centre origin, face normal, fixed direction set. But *which* faces own
  a probe is decided by a lattice in screen space, so an interpolation across neighbouring faces is
  a viewpoint-dependent answer to a viewpoint-independent question — which is why the lighting used
  to change as you flew around, and change systematically with distance. `resolve` now flags whether
  a sample came from this face, and `accumulate` treats the flag as authority: interpolated samples
  bootstrap a face and stop counting the moment a real one arrives. A face is then simply refreshed
  at irregular intervals with the same number, which is invisible.
- **The composite reads one value per face.** Every pixel on a face projects the same face centre
  through the same camera, lands on the same texel, and reads the same accumulated irradiance — so
  the indirect term is exactly flat across the face, by construction. Direct sun, emission, albedo
  and specular all stay per-pixel.

Everything read at a probe anchor is **point-sampled** (`pjvSnapToTexel`). Render targets are
filtered by default and probe anchors land on pixel boundaries, so an unsnapped fetch averages two
to four neighbours — which for a face centre yields a point on neither face, sliding continuously
as the camera moves, and for a face key yields an identity matching neither.

The flat lookup needs a check rather than trust: the lookup reads the key at that texel and requires it
to be this face, which rejects a face centre that is off screen, occluded, or too small to own its
own texel. That decision depends only on the face and the camera, never on the pixel, so a face is
never part flat and part filtered.

**Why not store per face directly?** Because the engine is fragment-only. A fragment shader writes
the pixel it rasterizes, so nothing can scatter a face's radiance into a slot indexed by its
identity — which is what a real surface cache needs. `materialListIndex` off the hit is already a
dense per-voxel integer that would address one. That is the next step, and it needs a pass type that
draws points with a custom vertex shader.

### Why the cascade gathers in world space

A screen-space gather ray cannot see off-screen geometry, so it cannot distinguish "blocked" from "sees sky" and defaults to sky — which paints a flat sky over everything. A world-space ray either hits a voxel (correctly occluded, and picking up that voxel's bounced sunlight and its own emission) or escapes the scene to the true sky. Sky occlusion and sky shadows come out right, at the cost of running the voxel DDA once per cascade texel.

The probes themselves are still anchored to the screen G-buffer. Only the rays are world-space.

### Why four cascades

Cascade *c* covers the interval `[start_c, start_c + len_c]` with `len_c = L0 * 4^c`, and holds `4^c` times as many directions over a quarter as many probes. Near light gets fine angular resolution where the geometry is close and the parallax is large; far light gets coarse angular resolution where it does not matter. Four levels reach roughly 85 × `L0`, which covers a room-scale scene. The knobs are at the top of `sharedShaders/pjv_cascade_common.sc`.

## Files

```
main.cpp                                  window, camera, scene load, per-frame uniforms
advancedRenderer/render.json              the ten passes and how they are wired
advancedRenderer/resources.json           uniforms, shader binaries, render targets
advancedRenderer/cascadeShaders/*.frag    the ten passes
sharedShaders/pjv_sun_sky.sc              the light rig (sun colour, sky gradient, sun-disk sampling)
sharedShaders/pjv_cascade_common.sc       cascade parameterization, octahedral directions, projection
sharedShaders/pjv_cascade_ws.sc           the world-space gather and the cascade merge
sharedShaders/pjv_face_key.sc             what "the same voxel face" means, in one place
```

The voxel traversal itself is not here — it is the engine's, at `include/pjv_utils_DDA.sc`, which every renderer in the tree includes. That is what keeps them all in step with a traversal change.

## How to build

```bash
cd AdvancedRenderer
make                # builds ./advanced_renderer
./compAdvanced.sh   # compiles the shaders to .bin
```

Requires ProjectV built at `../../../` (the repository root), with its libraries in `lib/`, bgfx built, and `build/tools/shadercRelease` present.

## How to use

```bash
./advanced_renderer                                    # opens ScenePreviewer/scenes/Untitled 3
./advanced_renderer "../ScenePreviewer/scenes/Untitled"
./advanced_renderer /path/to/any/compose/scene/
```

| Input | Action |
|-------|--------|
| `W` / `S` | Move forward / backward |
| `A` / `D` | Strafe left / right |
| `R` / `F` | Move up / down |
| `Shift` | Move 5× faster (hold) |
| Mouse | Look around (cursor is captured; `Esc` releases it, left-click re-captures) |
| Scroll wheel | Raise / lower the sun — a full day-night cycle, sunset colours included |

Two environment variables exist for reproducible captures and measurements, for the same reason the
scene editor has `EDITOR_START_MODE` — a screenshot cannot fly, and two builds are only comparable if
both measured the same view. `ADVANCED_CAMERA="x,y,z,yaw,pitch"` overrides the automatic framing, and
`ADVANCED_LOCK_CAMERA=1` ignores input entirely (the cursor is captured, so a stray pointer nudge
otherwise turns the camera mid-run).

The camera's start position and its flight speed are both derived from the scene's bounding box, because this example takes its scene on the command line and a hardcoded position would be inside one scene's wall and a kilometre from the next one's.

When the camera is still, the accumulate and TAA passes lengthen their running means and the image converges over a second or so. Any movement — including a sun change — switches them to their short reprojecting behaviour.

### A note on the scene library

This example ships no scene; it opens one from `../ScenePreviewer/scenes/`, which the previewer and the scene editor share. Several of those are older than the current `.data` container version and **load as zero chunks**, which the log says plainly:

```
loadComposeFromDisk: Loaded 0 chunk(s) ... from compose scene
readDataFile: ... is .data version 1, and only version 2 is supported. Re-voxelize this asset.
```

At the time of writing `Untitled`, `Untitled 2`, `Untitled 3`, `LittleBall`, `StonehillCastle` and `Bistro` load; `Sibenik` and `LostEmpire` do not. Re-voxelize a stale one with [MeshVoxelizer](../MeshVoxelizer/) to bring it back — that is how `Bistro` was brought back, and [ScenePreviewer's README](../ScenePreviewer/README.md#bundled-scenes) carries the command for each bundled scene.

## What changed from PathTracer

PathTracer's copy of this renderer does not run on the current engine. Four things had to change, and two more were worth changing while the file was open.

**Had to change:**

1. **Render-target sizing.** `resources.json` declared every texture `"resizable": true` with a hardcoded `resX`/`resY`. That flag is gone and now fails the load with an explicit migration message. Each target declares `"sizeMode": "relative"` with a `"scale"` instead, and is sized `ceil(scale × renderResolution)`. All sixteen here are `1.0`.
2. **`windowRes` / `texelSize` → `passTargetRes`.** Those were renderer-wide app uniforms, correct only while every pass shared one resolution. Since each pass now rasterizes at the size of *its own* target, the engine publishes `passTargetRes` = `(w, h, 1/w, 1/h)` per pass. A shader gets it by declaring it; it is not in `resources.json`. Everything measured in pixels — probe spacing, the direction-atlas layout, filter taps, sub-pixel jitter — reads it.
3. **Material lookup.** `fetchVoxelColor(hit.foundBox, hit.headerIndex)` rebuilds an integer voxel coordinate out of a world-space box. That float32 round trip loses low bits in proportion to the chunk's distance from the origin and shades a growing fraction of voxels with their *neighbour's* colour — measured at 6% for a small translation and 39% for a translation plus a rotation. `fetchVoxelMaterialFromHit(hit)` uses the material index the march already carried out: two texelFetches, no re-descent, and correct under any transform.
4. **The scene.** `SponzaScene/` is a version-1 `.data` container with a compose file that predates versioning, so it no longer loads at all. This example takes its scene as an argument instead of embedding one.

**Worth changing:**

5. **The expanded material system.** A voxel is no longer just a colour. `metallic` is subtracted out of the diffuse albedo, and `emission` is added in *two* places — to `gDirect`, so an emitter is bright on screen, and to the cascade gather, so it is an actual **GI light source**: the gather ray that lands on it carries its glow back to the probe, the merge spreads it up the cascade, and every surface that can see it gets brighter.
6. **The light rig is the scene editor's, term for term.** `pjv_sun_sky.sc` now takes the sun's intensity and the sky's from `renderParams`, and the sun's angular radius from `sunDir.w`, at the editor's default values. The primary ray's sun shadow samples that disk rather than aiming at a point, so shadow edges are as wide as the sun is and TAA resolves the penumbra. A scene lit here should read the same way in the editor's Render tab.

**Deliberately not carried over:** transparency. The editor's `albedo.frag` and `gi.frag` see through transparent voxels with `raySceneIntersectPeeled` / `raySceneTransmittance`; here every voxel is opaque. Peeling the cascade gather is a real piece of work — every one of its rays would pay for it — and it belongs in its own change.

## ProjectV features used

| Feature | Usage |
|---------|-------|
| **Core ECS** | `Application`, `world`, global resources, `Startup`/`Update`/`Render` stages |
| **Core Math** | `vec2`/`vec3`/`vec4`, `min`/`max`/`length` — camera, framing, uniform packing |
| **Logging** | `info`/`warn`/`error` — the scene report, and how a stale scene announces itself |
| **Compose I/O** | `loadComposeFromDisk` — a Compose folder flattened into a `Scene` |
| **GPU Interface** | `createTexturesForScene`, `GPUData` — the voxel scene as GPU textures |
| **Manage Resources** | `constructRendererSpecification`, `resizeRenderTargets` (via `renderConstructedRenderer`) |
| **Disk I/O** | `loadRendererSpecification`, `loadShader` |
| **Render Instance** | Window creation, renderer registry, active renderer |
| **Perform Renderer** | `renderConstructedRenderer`, `setUniformToValue`, per-pass view rects and `passTargetRes` |
| **Engine shader library** | `pjv_utils_DDA.sc` — `raySceneIntersect`, `rayStartDirection`, `fetchVoxelMaterialFromHit` |

## Tuning

Every knob is a `#define`, and each one names what it costs.

| Where | Knob | Effect |
|-------|------|--------|
| `pjv_cascade_common.sc` | `PROBE_SPACING0`, `DIR_TILE0` | Probe density and angular resolution. **A cascade occupies exactly `(D0/s0)²` of the screen, and the atlases are declared at that size** — at 16/4 that is `scale: 0.25` in resources.json. Change either and the scale must change with it. |
| | `CASCADE_BASE_LEN` | How far cascade 0 reaches, and so how far the whole stack does (`≈ 85 ×`). |
| | `SKY_GI` | Whether the sky lights the scene at all. |
| `pjv_cascade_ws.sc` | `WS_STEPS`, `WS_FINISH_LOD` | Gather-ray budget and how coarse distant geometry gets. |
| | `SUN_SHADOW_STEPS`, `SUN_BOUNCE_SHADOW_MAX_CASCADE` | The bounce shadow ray — the largest DDA cost in the renderer. |
| `gbuffer.frag` | `SHADOW_ORIGIN_BIAS_VOXELS` | Where the shadow ray leaves the surface, in voxel edge lengths. |
| `compose.frag` | `FLAT_MIN_FACE_PIXELS` | How large a face must be on screen before it gets one flat value instead of the per-pixel filter. |
| | `SPEC_STRENGTH`, `SPEC_SHININESS` | The rough specular reflection. Off by default (`SPEC_STRENGTH 0.0`). |
| `resolve.frag` | `FIREFLY_MAX` | Luminance cap on the indirect term. Matters more per-face: one bad probe is a whole face. |
| `display.frag` | `EXPOSURE`, `TINT`, `CONTRAST`, `SATURATION` | All colour grading. |
| `main.cpp` | `SUN_ANGULAR_RADIUS_DEGREES`, `SUN_INTENSITY`, `SKY_INTENSITY` | The light rig, at the scene editor's defaults. |

`resources.json` has one more, and it is the one the sizing rework above exists to make possible: drop every target's `scale` to `0.5` and the whole pipeline traces at a quarter of the pixels, while `display` — which writes the back buffer and so is sized by the window, not by a `scale` — still runs at native resolution and upsamples. The cost is that the upsample is plain bilinear, so the image softens.

Moving the scales **independently** is the interesting version and is not supported by these shaders yet: a low-resolution cascade read against a full-resolution G-buffer needs the G-buffer point-sampled through `passInputRes` and the indirect term brought back up with a joint bilateral upsample, the way the scene editor's `gi.frag` and `shade.frag` do it. Until then, move them together.

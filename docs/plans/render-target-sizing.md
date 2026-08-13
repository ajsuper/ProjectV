# Render target sizing: cleanup plan

Status: **all four steps landed.** Steps 1-3 were refactors that had to leave the image
unchanged; step 4 is the first one that changes what you see.

## Results

Verified by diffing the viewport against a baseline captured before any of this landed, on
`ScenePreviewer/scenes/Untitled 3`. The editor jitters per frame and averages 64, so two runs
are never bit-equal; the measure is mean absolute difference over the viewport crop.

| Step | vs baseline | verdict |
| --- | --- | --- |
| 1 defects fixed | mean 0.011/255, max 10, 5 px of 255150 above 8 levels | unchanged |
| 2 per-pass view rect | mean 0.010/255, max 11, 4 px | unchanged |
| 3 sizeMode schema | mean 0.012/255, max 12, 5 px | unchanged |
| 4 trace at 0.5 | changes by design | see below |

Step 4 confirmed the capability that did not exist before: the editor now runs seven passes at
640x360 and its display pass at 1280x720 in one renderer. Render mode, same panel resolution
and the same wall clock, 0 bounces on a mostly-empty frame: **6298 spp at scale 1.0 vs 11806
spp at 0.5, i.e. 1.87x more samples per second.**

Not the 4x the pixel count suggests, and the reason is worth writing down: that configuration
is not trace-bound. Most of the frame is sky, there are no bounces, and the fixed per-frame
cost (the interface, the full-resolution display pass, TAA, submit) does not shrink with the
trace. The closer the frame gets to being limited by rays -- geometry filling the viewport,
GI enabled -- the closer this gets to 4x. It is also why the half-resolution trade is worth
choosing per renderer rather than globally.

**The visible cost:** the half-resolution trace upscales soft, because `display.frag` samples
`accumColor` with plain bilinear filtering. That is the deferred work below coming due, and it
is one number per renderer to back out (`scale` in resources.json).

**Recommendation not yet acted on:** keep the *Edit viewport* at 1.0 and use 0.5 only in Render
mode. The viewport exists to judge stored voxel colour and shape, and it already runs at 0.8 ms
where Render mode is the mode that costs; softening the one view whose job is fidelity buys
little. Both are at 0.5 as requested -- changing the editorRenderer scales back is a one-line
edit per texture.

## Why

Size is not a property of anything in the render graph. It is a `(W, H)` pair threaded from
the frame loop into three places at once, and the only reason those three agree today is that
they are all handed the same argument:

```
        |- 1. output FBO's attachment sizes   <- resources.json (the only declarable one)
pass ---|- 2. its view rect                   <- setViewRect, GLOBAL for every pass in the frame
        |- 3. windowRes / texelSize           <- uniform, set once per renderer
```

`perform_renderer.cpp` calls `setViewRect(passID, 0, 0, windowWidth, windowHeight)` for every
pass regardless of which framebuffer it writes. So making one target smaller does not make its
pass rasterize smaller -- it rasterizes a full-size rect into a small target and gets clipped.
No amount of schema work fixes that; the rect has to come from the target.

The `resizable` bool is also already overloaded. Every texture in every `resources.json` is
declared `resizable: true`, including the 256x256 blue-noise images. Those stay 256x256 only
because they are `origin: CPUBuffer`, which routes them into `texturesResizedWithResourceTextures`,
whose sole reader (`gpu_interface.cpp:607`) is broken. They are not resized *by accident*.

## The model

A renderer has one **render resolution** per frame, decided by its driver. Every target is a
declared function of it. Size stops being an argument and becomes a resolved property.

```
driver picks render resolution
  -> each texture's sizing rule resolves to a pixel size
  -> each FBO's size = its attachments' size (validated equal at load)
  -> each pass's view rect = its OUTPUT FBO's size
  -> each pass's resolution uniform = the same
```

The back buffer leaves this system. It belongs to the driver, which is what forced the
SceneEditor to fork the engine's resizer in the first place (`main.cpp:2396` explains why:
the engine's version calls `bgfx::reset` with the size it is given, which would shrink the
back buffer to the viewport panel).

## Schema (clean break)

Two rules cover everything in the tree; nothing wants "relative to another target".

```json
{ "texID": 1, "name": "previewColor", "format": "RGBA16_FLOAT",
  "sizeMode": "relative", "scale": 0.5, "origin": "CreateNew" }

{ "texID": 9, "name": "blueNoise", "format": "RGBA8",
  "sizeMode": "fixed", "resX": 256, "resY": 256, "origin": "CPUBuffer" }
```

- `relative`: `scale` (default `1.0`) times the render resolution. `resX`/`resY` unread.
- `fixed`: `resX`/`resY` are the size, forever.
- Rounding is **ceil**, applied in one place in the engine. Consumers never assume a ratio --
  they read the actual size from `passTargetRes`.
- `resizable` is gone. `origin` keeps its real job (does the engine upload CPU data into this?)
  and stops secretly controlling size.

No compatibility shim, by decision: the other 10 `resources.json` files across PathTracer,
terrain_generator and ScenePreviewer will fail to load with an explicit migration message
naming the file and texture. They are migrated by hand later.

## Step 1 -- fix the defects in place

No schema change, no visual change. All in the engine's resizer
(`manage_resources.cpp:167`) and one neighbour.

| Fix | Defect |
| --- | --- |
| Store `textureFlags` at construction, reuse on recreate | resize recreates with `BGFX_TEXTURE_RT` only, silently dropping `BGFX_TEXTURE_READ_BACK \| BGFX_TEXTURE_BLIT_DST`, so a `resizable` + `readBack` texture stops being readable after the first resize (`:177`, `:195`) |
| `destroy()` the old handle before overwriting; `destroyTexture=false` everywhere | `:183`/`:202` assign over `frameBufferHandles[id]` without destroying it, leaking an FBO per resize, and pass `destroyTexture=true` where construction passes `false`, so the leaked FBO also claims ownership of textures the code destroys itself at `:174` |
| Update `textureResolutions` on resize | never updated, so the map is stale after any resize; only the editor's private copy maintains it (`main.cpp:2429`) |
| Rebuild only FBOs containing a changed texture | every FBO is rebuilt, including fixed-size ones |
| Drop `if (true && ...)`, drop the dead `resX`/`resY` locals | vestigial (`:168`, `:31-36`) |
| Real lookup + an actual `throw` | `gpu_interface.cpp:607-608` iterates an `unordered_map` by integer index (`map[i]` *inserts*) and compares the resulting `bool` against a texture ID, so `textureIsResizable` is meaningless; `:610` then constructs a `std::invalid_argument` without throwing it |

## Step 2 -- per-pass view rect + `passTargetRes`

Still one resolution everywhere, so every rect resolves to the value it already had and the
image is unchanged.

- `resolveTargetSize(renderer, fboID, backBufferRes)`: FBO `-1` -> back buffer, otherwise the
  first attachment's resolution. Store it in `BGFXDependencyGraph.windowWidth/windowHeight` --
  the fields set to `1`/`1` at `manage_resources.cpp:133` and never read. They were reaching
  for exactly this.
- `setViewRect` per pass from that.
- One engine-owned `passTargetRes` uniform, `vec4(w, h, 1/w, 1/h)`, set before each `submit`.
  bgfx snapshots uniform values at submit time, so a single name set per pass is correct --
  unlike `multiPassPassNumber<i>`, which is created per pass and which `denoise.frag` complains
  about for that reason.

`passInputRes[]` (per-input-slot sizes) is **deliberately deferred**. Step 4's configuration has
no cross-resolution reads inside the graph, so nothing needs it yet. It is what a mixed-resolution
config would need, along with a joint bilateral upsample.

## Step 3 -- the schema

Clean break, plus the editor migration:

- `Texture` gains `sizeMode` + `scale`, loses `resizable`.
- `disk_io.cpp` parses them and errors clearly on a file that still uses `resizable`.
- Load-time validation: every attachment of an FBO must have the same `sizeMode` and `scale`
  (or identical fixed dims), naming the `fboID` and offending `texID`s. This is what makes
  "an FBO has no size of its own" safe -- an inconsistent one cannot be declared.
- The editor's 2 `resources.json` move to the explicit form.
- The editor's 8 shaders move from `windowRes`/`texelSize` to `passTargetRes`. Every use site
  wants the pass's own grid (sub-pixel jitter, pixel coords for noise, a-trous tap offsets,
  the AO disc's radius-in-pixels), so it is a mechanical substitution. Aspect ratio is
  scale-invariant and unaffected.
- `resizeViewportTargets` (`main.cpp:2402`, ~65 lines) is deleted; the editor calls the engine's
  resizer, keeping only its own `bgfx::reset(glfwGetFramebufferSize(...))` for the back buffer.
  Two drivers, same functions, different render resolutions.

Also settle `getWindowResolution()`: it uses `glfwGetWindowSize` (logical) while the editor uses
`glfwGetFramebufferSize` (physical). Render targets and `bgfx::reset` both want physical. This is
the same divergence that the X11-under-Wayland fractional-scaling bug came from, and it returns
the moment a compositor scale is not 100%.

## Step 4 -- half-res trace

Set every editor pass except `display` to `scale: 0.5`. Then no read inside the graph crosses
resolutions -- `shade` reads a half-res `occlusion` into a half-res `shadedColor` -- and the
single upsample lands in `display.frag`, which already samples `accumColor` and is the natural
place for it. 4x fewer rays before anyone writes a bilateral upsample.

Mixed resolutions (full-res albedo, quarter-res GI) become possible but stay opt-in, and that is
when `passInputRes[]` and a joint upsample earn their keep.

## Found while implementing

Two things the plan did not anticipate, both now handled:

- **A relative target is born 1x1**, since its size is the render resolution times its scale and
  there is no render resolution at construction time. That made the editor's resize call, which
  was gated on "did the panel size change", never fire: `imgui.ini` restores the layout, so the
  requested size equals the current size on frame 1 and the targets stayed 1x1. `resizeRenderTargets`
  is now called unconditionally every frame -- it decides for itself whether anything needs doing,
  which is what it was designed for -- and its return value drives the accumulation reset. The old
  code only worked because targets used to be born at a plausible-looking declared resolution.
- **The scene passes are skipped for the one frame where the panel size is not yet known**, rather
  than tracing a placeholder-sized image. The interface still draws, which is what measures the
  panel, so the next frame renders properly.

## Follow-on: the viewport became a deferred path tracer

Landed after the four steps, and it is what the sizing work was for -- this configuration was not
expressible before, because every pass shared one resolution.

| Pass | Scale | Size at a 1280x720 viewport |
| --- | --- | --- |
| albedo (G-buffer: colour, normal, position, glow) | 1.0 | 1280x720 |
| gi (path trace) -> giRaw | 0.25 | 320x180 |
| gi_temporal (reproject + integrate) -> giLight | 0.25 | 320x180 |
| denoise x3 (a-trous, now RGB) -> giLight | 0.25 | 320x180 |
| shade (bilateral upsample + remodulate) | 1.0 | 1280x720 |
| accumulate, display | 1.0 | 1280x720 |

**Render mode is back to 1.0 throughout** -- it is the mode people screenshot, so it traces at
native panel resolution.

The split rests on one idea: **lighting is low frequency, albedo is not.** A voxel boundary is a
step in albedo, but the light either side of it is nearly the same, so tracing light per screen
pixel computes sixteen nearly-equal answers while the thing that actually varies is resolved for
free by the full-resolution G-buffer. So `gi.frag` traces at a sixteenth of the pixels and writes
**demodulated** irradiance -- the shaded surface's own albedo deliberately left out -- and
`shade.frag` multiplies the full-resolution albedo back in. Tracing the *product* at low resolution
would smear voxel colours into each other; a smooth light term times a sharp albedo does not.

Three details that are load-bearing rather than incidental:

- **The G-buffer is point-sampled, never filtered.** It is four times the tracer's width, so an
  ordinary fetch returns a bilinear blend of sixteen texels -- and a position or normal averaged
  across a silhouette is on neither surface, floating in front of one and buried in the other.
  Every ray from it would be wrong. Snapping the UV to one source texel's centre picks a real
  sample. This is what `passInputRes` exists for: a pass cannot point-sample an input whose size
  it has no way to learn.
- **The upsample is joint bilateral, not bilinear.** The four low-resolution samples around a pixel
  may sit on different surfaces; blending by distance alone drags the wall behind an object onto its
  edge, which is the halo every naively upsampled lighting buffer has. Each sample is weighted by
  how well the surface it was traced from agrees with this pixel's full-resolution surface, falling
  back to the centre sample when every neighbour is rejected -- slightly blocky beats haloed.
- **The advanced preview replaces the four readability aids rather than joining them.** They exist
  to make unlit geometry legible; once there is real light there is nothing to stand in for, and
  multiplying a fixed per-axis brightness or a screen-space occlusion term into a traced result
  would darken it twice for reasons the render will not reproduce.

The preview lights with **Render mode's rig, uniform for uniform** -- same sun direction and disk
radius, same bounce count, intensities and firefly clamp, through a shared `editorLightRig` helper
so the two cannot drift. That is what makes the viewport predict the render rather than resemble it.

The ray-traced ambient occlusion pass is gone: `occlusion.frag` is deleted and `gi.frag` took its
slot, its three denoise levels and its apply point. Occlusion is what GI does on the way past, so
the darkening survives and now arrives coloured, directional and with a sun in it. The cheap
screen-space AO toggle is untouched for the non-advanced path.

Measured at 1.5 ms (666 fps) with the preview on, against 0.8 ms with it off, on a viewport where
the traced light runs at 90x99. `EDITOR_ADVANCED_PREVIEW=1` turns it on at startup, for the same
reason `EDITOR_START_MODE` exists -- a screenshot cannot click.

### Temporal reprojection

`gi_temporal.frag` sits between the trace and the a-trous levels: it follows last frame's light to
where this frame's geometry has moved to on screen and blends this frame's samples into it. Without
it, averaging only worked while the camera was still -- the moment it moves, the pixel under a piece
of geometry is a different pixel and the running mean has to be thrown away.

Reprojecting the **demodulated light** rather than the final image is the point, and it is why this
sits here instead of in `taa.frag`. A reprojected colour buffer drags a voxel's colour a fraction of
a pixel every frame the camera moves, and that error is a smear along the direction of travel that
never settles. Here the albedo is not in the buffer at all -- `shade.frag` applies it fresh from this
frame's G-buffer -- so a slightly stale reprojection produces slightly stale *lighting*, which is low
frequency and invisible, while every edge stays where this frame's geometry says it is. It also means
the validation can be one number instead of a neighbourhood colour clamp.

Mechanics worth keeping straight:

- **The history is the filtered buffer.** This pass and the three a-trous levels all write `giLight`,
  and this pass reads it, so the history is the previous frame's *filtered* result rather than its raw
  trace. That is the SVGF arrangement and it is what makes convergence fast. The engine detects the
  ping-pong automatically because the pass names its own output among its inputs.
- **Validation is a depth key in alpha**: how far the shaded point sits along the view axis, written
  against the camera that wrote it and compared next frame against the same quantity recomputed
  against that same camera. A disocclusion fails it, which is what it is for. The three denoise levels
  carry alpha through *unfiltered* -- averaging the depths of every surface in the kernel would produce
  a number belonging to none of them.
- **A fixed 12-frame window** (`HISTORY_ALPHA`) rather than a running sample count, which would need a
  channel to live in. The accumulate pass already does the long-window averaging for the still case;
  what this pass has to be good at is the case that one cannot help with.

Verified by instrumenting the pass to report which branch each pixel took: on a settled camera
**98% of geometry pixels reuse history**, 0.1% are rejected for surface mismatch (silhouette edges,
where the key legitimately disagrees) and 0.0% land off screen. Worth doing that check rather than
trusting a screenshot -- a bug that rejected every sample would look identical in a static frame.

Costs about 0.1 ms: 1.6 ms with it against 1.5 ms without.

**Known rough edge:** the viewport background is still albedo.frag's neutral dark backdrop, not
`skyGradient`. With a sky now lighting the scene, a preview of the render arguably wants the sky
behind it too. Left alone because the backdrop is deliberately neutral for judging stored colour,
and changing it is a separate decision.

## Still deferred

- ~~`passInputRes[]` and a joint upsample~~ -- both landed with the deferred path tracer above.
- ~~A temporal component on the traced light~~ -- landed, see above.
- The other 10 resources.json files (PathTracer x7, terrain_generator x2, ScenePreviewer x1) still
  declare `resizable` and now fail to load with a message naming the file, the texture, and the
  replacement. Migrating by hand, deliberately.
- `getWindowResolution()` still uses `glfwGetWindowSize` (logical) while the editor uses
  `glfwGetFramebufferSize` (physical).

## Verification

Steps 1-3 are checked by pixel-diffing the viewport against a baseline captured before any of
this landed (`BASELINE_viewport.png`), on `ScenePreviewer/scenes/Untitled 3` with the editor's
reproducible auto-framing. Any difference in steps 1-3 is a bug, not a trade-off. Step 4 changes
the image by design and is judged on frame time and on how the reconstruction looks while flying.

## Context

- The renderer was measured at ~540 M coherent primary rays/sec, which is healthy for software
  DDA. At 3328x1771 one primary ray is ~7 ms, so a native-resolution frame has room for roughly
  two rays per pixel at 60 fps. Resolution is the dominant lever; traversal micro-optimisation
  is worth maybe 2-3x against this 4x.
- Related traversal work already landed: the march now carries the material index out
  (`SceneIntersectData::materialListIndex`), so a material lookup is two texelFetches instead
  of a chunk-header fetch plus a root-to-leaf descent.
- Still outstanding on the traversal side, in value order: bound rays by distance (an escaping
  shadow/GI ray currently walks the whole scene with a fresh 256x12 step budget per grid cell),
  then make peel layers cheaper and fewer.

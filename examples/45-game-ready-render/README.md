# 45 — Game Ready Render

A renderer to put under a game: cheap, steady, and good-looking at well over 120 fps. It grew from the
[physics demos'](../17-physics-demos/) renderer — a G-buffer, SSAO, a shade pass and the voxel
reconstruction — and adds what that one lacked for a finished image: TAA, a real light rig, one-bounce
ray-traced GI at a sixteenth of the render pixels with screen-space AO and colour bounce for the near
detail, height fog and a grade.

**Cleanliness over fidelity** is the rule it is tuned to. The sub-pixel jitter is off by default: the
reconstruction antialiases every edge it can see from one frame, and without the jitter a still frame is
identical to the last, so nothing shimmers. (`E` turns it on; it buys sub-render-pixel detail at exactly
that price.)

Where [40-advanced-renderer](../40-advanced-renderer/) asks how good a voxel frame can look, this one
asks how good it can look at a game's frame rate. The one stochastic term is the ray-traced GI, and it
is accumulated over frames and blurred until it is not seen as noise; everything else is deterministic.

## The pipeline

Ten passes. Everything before `reconstruct` runs at the render scale (0.4 of the window on each axis
by default, 16% of the pixels); the four GI passes at half of that again on each axis.

| # | Pass | Resolution | What it does |
|---|------|------------|--------------|
| 1 | `gbuffer` | render | One primary ray and one sun shadow ray per pixel, through **one** call site of the engine's geometry-only `raySceneIntersectFrom` (a second inlined copy of the march slows every ray). Every hit is a real voxel (no primary-ray LOD: at a reduced render scale it shows as a visible change of detail). All voxels are opaque. |
| 2 | `ssao` | render | AO at two scales, **contact** (a few voxels) and **wide** (a few metres), and a one-bounce **colour bounce**, from the same samples. The samples are surface points read back from the G-buffer in a spiral round the pixel, each occluding by its elevation above the pixel's tangent plane (Alchemy/HBAO-style), so slanted faces never occlude themselves and convex edges get no dark rim. Each blocked sample also returns the light leaving it (albedo, sun, shadow, emission). Turned by a static 4×4 pattern. |
| 3 | `raygi` | render ÷ 2 | One cosine-weighted voxel ray per pixel (192 voxels long), turned each frame along an R2 sequence. A miss takes the sky's radiance in that direction; a hit lights what it hit with a shadow ray to the sun, a little ambient and its emission. Both rays go through one call of the march. |
| 4 | `giaccum` | render ÷ 2 | The GI's own history: each pixel's surface point reprojected through the previous camera, its average carried on only where the stored point still matches (up to 24 frames). What moved starts again. |
| 5 | `giblur1` | render ÷ 2 | 5×5 à-trous (B3 spline) at stride 1, stopped where the surface turns or leaves the plane; pixels with little history lean on their neighbours. |
| 6 | `giblur2` | render ÷ 2 | The same at stride 3: together a wide, soft, edge-aware blur. |
| 7 | `shade` | render | `albedo × (sun × N·L × shadow + rayGI × contactAO + bounce) + emission`, a small Fresnel sheen, then height fog. The ray GI is upsampled with the same surface tests. The AO and bounce are filtered 5×5 with weights [1 2 2 2 1] per axis -- every rotation of the 4×4 pattern equally, so it cancels -- and the hard shadow over a same-surface ring. With the ray GI off (`I`), the ambient is `grrAmbient(N) × contactAO × wideAO` as before. |
| 8 | `reconstruct` | output | The physics demos' voxel reconstruction, unchanged: every render texel that saw a voxel knows the whole cube, so each output pixel is composited from how much of it each nearby voxel covers. This is the upscale *and* the edge antialiasing. |
| 9 | `taa` | output | Reprojects through last frame's camera, Catmull-Rom history fetch, variance clip in YCoCg, luminance-weighted blend. Steadies motion. |
| 10 | `display` | output | Exposure, vignette, ACES, gamma, a light contrast/saturation trim, dither. |

### The GI

Two scales, each doing what it is good at:

- **Large scale, traced** (`raygi` → `giaccum` → `giblur1/2`). What the sky and the world round a point
  actually send it: a courtyard's floor and walls go dark and cool because the sky they see is small, the
  base of a wall warms from the sunlit ground in front of it, an emissive voxel lights its surroundings,
  and light from off screen or behind the camera counts. One ray per 2×2 render pixels; the history and
  the blur turn that into a smooth field. Costs about 0.5–0.9 ms on an RTX 3070 at a 0.5 render scale.
- **Small scale, screen space** (`ssao`). Contact AO and the colour bounce of what is right beside a
  point -- detail the quarter-resolution rays blur away. With the ray GI on, the bounce is halved so the
  near light is not counted twice, and the wide AO is not used.

The limit is anything that moves. There are no per-object motion vectors, so a moving body's surface
fails the history test every frame and its GI is one frame's ray, blurred, until it stops; and light
takes a few tenths of a second to catch up when the scene changes. `I` turns the traced GI off, back to
the old cheap ambient: `grrAmbient` (sky above, sun bounced off the ground below) times the two-scale
AO, which costs no rays and knows nothing about colour bleeding or off-screen light.

### Shadows

The shadow ray coarsens with distance (LOD 2 by 32 voxels **away from the surface**). The ramp is
stretched by 1/(N·L): merged LOD boxes stick out of the wall they belong to, and a ray grazing up a wall
on a ramp measured along the ray hits them and paints the wall's node grid on it as false shadow.
Coarsening cut the shadow ray's cost by more than half under the tree canopy in SanMiguel.

## How to build and run

```bash
cmake --preset dev
cmake --build --preset dev --target game_ready_render
cd build/examples/game_ready_render
./game_ready_render                                   # Cave, on the ground in the meadow
./game_ready_render ../scene_previewer/scenes/SanMiguel/   # the heavier test
```

| Input | Action |
|-------|--------|
| `W` `A` `S` `D`, `R` / `F` | Fly; `Shift` ×5 |
| Mouse | Look (`Esc` frees the cursor, left-click recaptures) |
| Scroll | Sun elevation, through sunset |
| `Z` `X` `C` `` ` `` | Render scale 1.0 / 0.5 / **0.4** / 0.33 |
| `Q` | Reconstruction bypass (raw render resolution, point sampled) |
| `T` / `E` | TAA on/off / sub-pixel jitter on/off |
| `G` / `H` / `V` | AO on/off / sun shadows on/off / fog off, normal, heavy |
| `I` | Ray-traced GI on/off |
| `J` | Screen-space colour bounce off / normal / strong |
| `U` | Debug view: off / reconstruction coverage / AO only / raw sun visibility / colour bounce (×4) / ray-traced GI |
| `Y` | Per-pass GPU timing to the log every 120 frames |
| `P` | Print the camera as a `GAME_CAMERA="…"` line |

For scripted runs: `GAME_CAMERA="x,y,z,yaw,pitch"`, `GAME_LOCK_CAMERA=1`, `GAME_RENDER_SCALE`,
`GAME_SUN=<scroll notches>`, `GAME_FOG=<multiplier>`, `GAME_SHADOWS`/`GAME_AO`/`GAME_TAA=0|1`,
`GAME_RAYGI=0|1`, `GAME_BOUNCE=0..2`, `GAME_DEBUG_VIEW=0..4|7`, `GAME_PROFILE=1`, `GAME_BENCH=<seconds>` (with `GAME_BENCH_SPIN=<rad/s>` to
turn the camera) and `GAME_CAPTURE=<file.ppm>`, which writes the frame and exits.

## What limits the speed

Measured with the engine's march counters (`GRR_STATS` in `gbuffer.frag`, read back with
`GAME_CAPTURE_TEXTURE=3`), a ray here does very little traversal:

| Per ray | Cave primary | Cave shadow | SanMiguel primary | SanMiguel shadow |
|---|---|---|---|---|
| Tree-loop iterations, mean / p99 | 9 / 29 | 8 / 21 | 16 / 46 | 8 / 22 |
| Chunks entered | 1.3 | 2.0 | 2.4 | 2.1 |
| Grid cells walked | 1.3 | 2.0 | 2.4 | 2.2 |

Every grid cell a ray walks holds a chunk, so there is no empty space at the scene level to skip; a
BVH over the grid, or starting rays nearer their surface, would save almost nothing. The cost per ray is
fixed overhead and shader size, and the biggest share of that was the engine's full query.
`raySceneIntersect` wraps the march in a transparency/refraction/material loop that is compiled in
whether a ray uses it or not; switching to the geometry-only `raySceneIntersectFrom` cut the G-buffer
from 2.19 to 1.21 ms on Cave and from 4.74 to 2.60 ms on SanMiguel. Turning transparency off while
keeping the full query saved nothing. The price is that every voxel is opaque.

The reconstruction is limited by texture fetches rather than arithmetic: a fast path that skipped the
coverage maths for face interiors measured slower, because its own nine reads cost more.

## Performance

*Measured before the ray-traced GI and the new AO replaced the far filter; the GI adds about
0.5–0.9 ms (`raygi`) and 0.4 ms (`giaccum` and the two blurs) on the same card.*

RTX 3070, default 0.4 scale. The compositor is currently pacing this window to the display (~149 fps
whatever the GPU does), so the GPU per-pass time is the number that means anything. 1273×668 window:

| Pass | Cave meadow | SanMiguel courtyard |
|------|------|------|
| gbuffer | 0.74 ms | 1.6 ms |
| reconstruct | 0.44 ms | 0.45 ms |
| taa | 0.14 ms | 0.14 ms |
| ssao + shade + farfilter + display | 0.18 ms | 0.19 ms |
| **total** | **~1.5 ms** | **~2.4 ms** |

In a 1273×1370 window (twice the pixels) Cave measured 1.2 ms G-buffer and 0.8 ms reconstruction.

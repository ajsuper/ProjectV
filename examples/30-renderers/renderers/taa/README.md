# taa — temporal reprojection and accumulation

`--renderer taa`

The temporal filtering demo. It is built on a path tracer, but **the path trace is the input, not
the subject** — what this shows is how to reuse the previous frame's result: project this frame's
world-space hits back through the previous frame's camera, decide whether the match is real, and
blend. That machinery is what every temporal antialiaser and temporal upscaler is made of, and it
is the technique this renderer exists to teach.

AdvancedRenderer used to carry a TAA pass of its own. It doesn't any more — this is where the
technique lives.

## The problem it solves

[tree64](../tree64/) converges beautifully and then throws all of it away the moment the camera
moves, because a sample taken from one viewpoint says nothing about another. That is correct and it
is unusable in motion.

Reprojection says: it *does* say something, as long as you can work out where that sample landed
this frame and check that it is still the same surface. Get that right and a moving camera keeps
most of its history instead of resetting to noise.

Getting it right is the whole difficulty, and it is three separate problems:

**Where did this pixel used to be?** `worldToUV` in `reproject_accumulate.frag` is the exact inverse
of the engine's `rayStartDirection` — deliberately, because a reprojection that disagrees with the
ray generation by even a little smears. The primary ray here is **deterministic**, with no jitter,
so each pixel locks to one surface point and the history lookup is exact rather than approximate.

**Is it still the same surface?** A pixel can reproject onto a valid-looking texel that belongs to
something else entirely — the classic disocclusion, where a foreground object moves and reveals what
was behind it. The G-buffer is noise-free, so position and normal can be compared directly and a bad
match rejected outright rather than blended in at reduced weight.

**How much of the past should count?** A *motion-adaptive* running average: still camera, the
history grows very long and noise converges toward zero; moving camera, the history is kept short so
the image stays responsive and does not trail. History is point-sampled at the nearest texel, because
bilinear history resampling loses a little sharpness every frame and the losses compound.

## Passes

`render.json`, four passes:

| # | Pass | What it does |
|---|------|--------------|
| 1 | `path_trace` | One sample per pixel — cosine-weighted diffuse GI plus next-event estimation to the sun. Writes a G-buffer of radiance, world position, normal and albedo. Deterministic primary ray. |
| 2 | `reproject_accumulate` | The technique. Reprojects, validates against the G-buffer, folds the sample into a ping-pong history with a motion-adaptive average. |
| 3 | `denoise` | Edge-aware à-trous filter guided by the G-buffer. Cleans what a deliberately short moving-history leaves behind, so motion is sharp *and* clean without leaning harder on the temporal blend. |
| 4 | `display` | ACES tone map and gamma. |

The history buffer is a ping-pong framebuffer — a pass that both reads and writes framebuffer 2 —
which the engine detects and handles on its own.

## What to look at

Fly, then stop. The image should already be close when you stop rather than resolving from noise,
which is the difference between this and [tree64](../tree64/) on the same scene.

Then look for the failure modes, because they are the reason each piece exists: strafe past a near
edge and watch the disocclusion band behind it — that is the validation rejecting history and the
denoiser covering the gap alone until the average rebuilds.

## Files

```
render.json       Four passes: path_trace, reproject_accumulate, denoise, display
resources.json    Shaders, render targets, framebuffers (framebuffer 2 is ping-pong)
shaders/          The four fragment shaders plus the fullscreen-quad vertex shader
```

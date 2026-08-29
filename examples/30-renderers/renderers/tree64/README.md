# tree64 — accumulation path tracer

`--renderer tree64`

The original ProjectV path tracer. It accumulates samples across frames while the camera is still and **resets accumulation whenever the camera moves**. Its four-pass pipeline (`render.json`):

1. **path_trace** — casts rays into the tree64 voxel structure and accumulates radiance samples
2. **accumulate** — blends the current frame with the history buffer when the camera is still
3. **denoise_ao_gi** — applies a denoising pass over ambient occlusion and global illumination
4. **post_and_display** — tone-maps and outputs the final image to the screen

## When to reach for it

It is the simplest thing here and the baseline the others are measured against: a still camera
converges to a reference image, so this is what to compare a faster renderer's output *to*. The
hard reset on camera movement is the whole trade — you get correctness while parked and noise the
moment you move, which is exactly the problem [reprojection](../reprojection/) exists to solve.

## Files

```
render.json       Four passes: path_trace, accumulate, denoise_ao_gi, post_and_display
resources.json    Shaders, render targets, framebuffers
shaders/          The four fragment shaders plus the fullscreen-quad vertex shader
```

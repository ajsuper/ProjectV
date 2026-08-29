# fast — direct lighting + denoised RTAO, TAA-antialiased

`--renderer fast`

A deliberately cheap, (almost) noise-free renderer for when you want a **reliable, performance-friendly** image rather than full global illumination. It casts **no random GI bounces** — every pixel is shaded from a single primary ray plus one hard sun shadow ray, so the raw image is already stable before any temporal work. Ambient occlusion is **ray-traced** (RTAO) but kept cheap by tracing only a couple of rays per pixel and denoising them. Five passes (`render.json`):

1. **gbuffer_shade** — one primary ray (sub-pixel *jittered* each frame, Halton), shaded with a single **hard sun shadow ray** (crisp deterministic direct light, no soft-shadow noise). It writes a G-buffer with the direct sun light and the sky-ambient term on **separate** targets (plus world position + normal), so AO can occlude the ambient without dimming sunlit surfaces. The sun shadow ray uses a visibility-only trace that skips the expensive per-hit voxel-colour decode (`fetchVoxelData`).
2. **rtao** — ray-traced ambient occlusion (deferred). From each pixel's G-buffer position it casts a few short hemisphere occlusion rays through the voxel DDA. Unlike screen-space AO this is (near) ground truth — it sees occluders that are **off-screen or hidden behind silhouettes**, which is why it reads noticeably better on this blocky geometry. The catch is cost: hemisphere rays are incoherent and the DDA (`pjv_utils_DDA.sc`) has no working LOD, so every ray marches at full leaf resolution and a *single* 24-step AO ray costs ~2× a 128-step sun ray. It's kept affordable the way real-time RTAO is: only **`AO_SAMPLES` rays per pixel per frame** (default 2, a visibility-only trace), with the noise removed by the denoiser below + TAA rather than by brute-force sampling. (An earlier revision used screen-space AO here for speed, but with the denoiser in place RTAO costs about the same and looks better, so it was restored.)
3. **combine_blur** — an **edge-aware bilateral blur** of the raw AO, the spatial half of the denoiser. Weights are stopped on G-buffer depth and normal so occlusion is averaged only across coplanar neighbours (creases/silhouettes stay crisp, flat areas go smooth); because each neighbour pixel traced a *different* rotated direction, a 5×5 blur is worth ~25 effective AO directions. It then applies the blurred AO to the sky-ambient term only (`direct + ambient·ao`) and carries the position/normal forward. The AO ray directions are rotated per pixel (interleaved-gradient noise) *and* per frame, so this blur plus the temporal pass fully resolve the few-ray noise.
4. **taa** — temporal anti-aliasing. When the camera is **still** it accumulates each pixel's own history (identity, texel-snapped, no resampling blur) into a long running mean, so the per-frame jitter and AO rotation resolve into supersampled, crisp, noise-free edges (and the AO converges toward many-sample quality). When the camera **moves** it reprojects through the previous camera, samples history bilinearly, and **clamps it to the local 3×3 colour neighbourhood** (the deghoster) with a short history so motion stays clean and responsive.
5. **display** — ACES tone-map + gamma to the screen.

The only stochastic elements are the sub-pixel jitter (for AA) and the per-frame AO ray rotation (which the denoiser + TAA converge); the lighting itself is deterministic, so a still camera reaches a clean image in a fraction of a second. Per pixel it issues `2 + AO_SAMPLES` scene rays. `AO_SAMPLES` (top of `rtao.frag`) is the quality/perf knob — 1 is about as cheap as the old SSAO, 2 (default) is cleaner, higher is cleaner still. Its TAA reuses the same `worldToUV`/ping-pong reprojection machinery as renderer 2.

## When to reach for it

When you want a reliable image at a predictable cost rather than a correct one. It casts no random
GI bounces at all, so there is nothing to converge: the raw frame is already stable, and the
temporal machinery is there for anti-aliasing and AO denoising rather than for noise reduction.

It is also the renderer [10-scene-previewer](../../../10-scene-previewer/) was cut down from —
strip everything except the primary march and you have the previewer.

## Files

```
render.json       Five passes: gbuffer_shade, rtao, combine_blur, taa, display
resources.json    Shaders, render targets, framebuffers
shaders/          The five fragment shaders plus the fullscreen-quad vertex shader
```

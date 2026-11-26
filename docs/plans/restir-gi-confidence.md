# restir-pt: history that adapts per pixel, and a history fix for what was uncovered

Proposed 2026-11-12. Status: **built** (C1, C2; agreed with the user the same day). Thresholds open to tuning by eye.

restir-pt's GI accumulation (`gi_accumulate.frag`) keeps a fixed history everywhere -- 12 frames of diffuse --
and shortens it everywhere at once when the sun moves (`RESTIR_SUN_MOVING_HISTORY`). Changing light (the sun,
an explosion, a moving body's bounce) therefore lags by the cap everywhere, and a cap long enough to be quiet
is a cap that lags. And a pixel with no history -- uncovered by the camera's motion -- reaches the a-trous with
one frame of paths, which the filter turns into blotches. Two denoiser-only changes, no rays, from what the
shipping implementations do (kajiya's `rtdgi/temporal_filter.hlsl`; NRD's history confidence and HistoryFix).

## Decisions

**D1. A per-pixel confidence from how far the history is from this frame.** In `gi_accumulate`, after the
history is reprojected: the mean of this frame's light over the 3x3 GI pixels of the same surface, against
the history. Their difference measured in the history's own noise -- the temporal standard deviation its
moments already carry, shrunk for the mean of a few correlated neighbours -- is a z-score; past
`RESTIR_CHANGE_FROM` deviations the history is not this light any more, and it is cut towards
`RESTIR_FAST_HISTORY` frames by `RESTIR_CHANGE_FULL`. Per lobe. kajiya does the same (its cap falls from 32
to 4 on `temporal_change / temporal_dev`); NRD takes a confidence that costs re-traced rays, which this does not.

**D2. With it, the caps can be longer and the sun's global cut goes.** A still, unchanged pixel keeps more
history (the diffuse cap 12 -> 24 frames: less noise), and a pixel whose light changed shortens alone. The
GI's `RESTIR_SUN_MOVING_HISTORY` cut is dropped; the reservoirs' M cut stays (their cached light is a frame
old, and nothing measures it), and so do the shafts' and the glossy reflections' (their own passes).

**D3. A history fix before the a-trous.** A new pass, `gi_history_fix`, between `gi_accumulate` and `atrous1`:
a GI pixel with under `RESTIR_FIX_FRAMES` frames of history is replaced by a disc blur over the same surface
(normal, plane; the specular also the same glossiness), as wide as `RESTIR_FIX_RADIUS` GI pixels with no
history and narrowing as it grows, each tap weighted by its own history length -- so an uncovered pixel takes
the settled light beside it rather than its own one frame. NRD's HistoryFix. Pixels with history pass through.

**D4. Not changed: the spatial reuse.** It already reaches twice as far and takes the most neighbours for a
fresh reservoir (`spatial.frag`, `fresh`) -- kajiya's widening for a low M.

## Milestones

| # | What | Done when |
|---|---|---|
| C1 | D1, D2 | after a sun step the indirect settles in fewer frames than before, and a still view is no noisier |
| C2 | D3 | a sidestep's uncovered strip is less blotchy in its first frames; cost measured |

## Open questions

1. The thresholds (`RESTIR_CHANGE_FROM` / `_FULL`), set by measurement and then by eye.
2. Whether the image's resolve could use the same signal (kajiya narrows its clamp box with it). Not here.

## As built

- **C1** as D1/D2, with `RESTIR_FAST_HISTORY` 3, `RESTIR_CHANGE_FROM` 2 and `_FULL` 5 deviations, a floor of 3%
  of the light, and the 3x3 mean's deviation taken as 0.6 of one pixel's. `GI_HISTORY` (`restir_pt.h`) 12 -> 24.
- The sun's move now reaches the shaders as its size (`frameCount.z`: 1 a step under `SUN_JUMP_DEGREES` = 2 degrees,
  2 a jump). The reservoirs, the shafts and the image start over on a jump only -- the reservoirs' cached light
  is never re-traced (validation is compiled out), so after a jump it stayed wrong for dozens of frames.
- **C2** as D3: `RESTIR_FIX_FRAMES` 4, `RESTIR_FIX_RADIUS` 6 (+1) GI pixels, 16 taps.
- Measured on Frustrum's indirect view (`RESTIR_VIEW=1`, the water masked out), the frames at the `--look`
  pose under `GALLERY_WOBBLE`, mean change between them (0-255):

  | | walking (1) | running (4) |
  |---|---|---|
  | before | 0.446 | 0.740 |
  | C1 | 0.383 | 0.694 |
  | C1 + C2 | 0.376 | 0.672 |

  A still view's frame-to-frame change was 0.021 before and after (the image's 64-frame history dominates it).
  Cost at 1280x720: `gi_accumulate` 0.04 ms, `gi_history_fix` 0.02 ms.
- A sun step's settling could not be told apart this way: after a jump the image's resolve restarts and then
  averages the GI's first frames for 64, which dominates it.

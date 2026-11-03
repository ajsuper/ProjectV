# restir-pt: a performance preset, and optimizations

Status: **delivered** (2026-11-03): P0-P2 built, performance remade clean (D7-D9), approved by the user; **P3 deferred** (the user: graphics are fine as they are). Asked for: restir-pt is unusable on the user's laptop; today's
configuration becomes the **quality** preset, and a simple **performance** preset is added, plus
optimizations. The user's target for performance: **60 fps on the laptop, a softer image accepted**.

The laptop: AMD Radeon 860M (RDNA 3.5 integrated, RADV), a 2880 x 1920 panel at 170%. The quality
preset was tuned on the desktop (NVIDIA; "about 30 fps on the island", island-demo.md I1).

## Where the time goes now

`renderer_gallery --renderer restir-pt --scene <island-42> --bench`, 1280 x 720, the laptop (2026-11-03):

| | ms | |
|---|---|---|
| **frame** | **40.9** | |
| `gbuffer` (½ x ½: camera ray, sun shadow ray) | 9.4 | |
| `sample` (¼ x ¼: a new path, temporal reuse) | 13.4 | 2 bounces, NEE at each |
| `spatial` (¼ x ¼: 5 neighbours, a visibility ray) | 7.0 | |
| `reconstruct` + `resolve` (window) | 3.9 | |
| everything else (21 passes) | 7.2 | `shadow_soft` 0.9, `composite` 0.7, `shafts` 0.7, `dof` 0.6 ... |

Tracing is 30 of 41 ms. At the island's real window (1600 x 900 at 170%: a 2720 x 1530 back buffer,
4.5x these pixels) the frame would be several times longer. No pass list or format change reaches
16.7 ms alone: performance has to **trace fewer rays at fewer pixels**.

### P0: each lever alone (2026-11-03)

The same, at the island's spawn (`--look 93.10,12.42,80.71,83.15,11.43,80.71`), one batch, the gallery's
`RESTIR_*` switches (uniforms: a pass switched off this way still runs):

| | frame | `gbuffer` | `sample` | `spatial` | |
|---|---|---|---|---|---|
| quality | 40.2 | 6.9 | 14.0 | 8.3 | |
| `RESTIR_BOUNCES=1` | 32.6 | 6.9 | **6.9** | 7.9 | a bounce is half of `sample` |
| `RESTIR_SPATIAL=0` | 32.0 | 6.9 | 13.9 | **0.4** | |
| `RESTIR_VISIBILITY=0` | 34.5 | 6.9 | 13.9 | **2.5** | the visibility ray is 70% of `spatial` |
| shafts, soft shadow, SSAO, bloom, motion blur off | 38.7 | | | | 1.5 ms: the passes still run (D4) |
| `RESTIR_REUSE=0` | 31.7 | 6.9 | 13.5 | 0.4 | temporal reuse costs 0.5 |

A secondary ray costs about four camera rays (`sample`: ~60 ns a ray, `gbuffer`: ~15): long, divergent
rays across the island's 1536 voxels. Estimate for performance in the island (its back buffer is the
window's logical size, 1600 x 900; half: 800 x 450, a 400 x 225 G-buffer): `gbuffer` 2.7, `sample` 2.7,
`spatial` 0.6, the window passes 1.5, the rest about 1.5 with D4's passes off: **about 9 ms**.

## Decisions

**D1. Two presets, chosen by the program.** `restir_pt::Preset { Quality, Performance }` and
`restir_pt::settingsFor(Preset)`: quality is today's `Settings`; performance is D3-D5. The gallery's
panel and `--preset`, the island demo's menus (start screen and Esc) and `--graphics quality|performance`
switch them at runtime. *Why:* one renderer folder, one set of shaders; a preset is uniforms, enabled
passes and a render scale (D2).

**D2. The engine gains a render scale** (`graphics/render_instance.h`): window-relative targets are sized
from the window times the scale, and the passes that write the back buffer still cover it (they sample
their inputs by UV, so they upscale). A mechanism, not policy: the program sets it. Performance draws at
**0.5** (the island's window: 1360 x 765 out, a 680 x 383 G-buffer, paths for 170 x 96). *Why:* the
largest lever there is, and the targets' sizes are already window-relative (`resizeRenderTargets`), so it
is one number in the driver. A change of scale is a resize: histories restart, as now.

**D3. Performance traces less.** One bounce (`bounces` 1), spatial reuse from 2 neighbours instead of 5
with no visibility ray, the camera ray's sun shadow ray coarse past 16 voxels (as the paths' sun is now),
and water and mirrors reflected in **screen space** (a march through the G-buffer's depth, the sky where
it leaves the screen) instead of `reflect`'s traced ray -- the user's choice. *Why:* the three tracing
passes are three quarters of the frame.

**D4. Performance switches passes off** with the passes' existing `enabled` flag, behind a call by name
(`graphics::setPassEnabled(renderer, "dof", false)`): depth of field, motion blur, bloom, light shafts,
SSAO, the a-trous steps at 4 and 8. *Why:* each is under a millisecond at 720p, together about 3, and a
disabled pass costs nothing (an early-out in the shader still writes every pixel).

**D5. Optimizations for both presets** are kept only if the quality preset's image is unchanged within
the shader library's tolerance (`projv_imgdiff`) or the user approves the difference. Candidates, each
measured on its own:

| | Expected | Risk |
|---|---|---|
| The camera ray's sun shadow ray coarse past 64 voxels (quality) | part of `gbuffer`'s 9.4 | a thin far shadow lost |
| Smaller formats: `gPos`, `gDirect`, `gFace`, `resolved` are RGBA32F | bandwidth (an iGPU's weak spot) | precision: positions far from the origin |
| `spatial`: skip a neighbour whose surface fails the normal/depth test before shifting it | part of 7.0 | none (it is refused now, later) |
| `sample`: paths end early over the sky and at low throughput (Russian roulette) | part of 13.4 | noise |

**D6. The island demo picks performance on an integrated GPU**, and the choice is remembered beside the
program (`settings.json`, next to `worlds/`). *Why:* the laptop is the machine it is unusable on; bgfx
reports the vendor and device, and RADV/Mesa names an integrated part (the island already reads
`bgfx::getCaps`). Open question 2.

**D7. Performance is clean, not rich** (2026-11-03, after the user's look at P2: "very not clean" -- blotchy
noisy light, blurry, shimmer and smear in motion; "a clean image over a complicated one"). Its lighting is
**simple**: no paths -- the sky over the normal (60% of it: hills and trees hide the rest) and the sun
bounced off the ground below, shaded by an SSAO four times as wide and at full strength -- so nothing is
noisy, and the path passes and their denoiser are skipped. The time that frees buys **three quarters** of
the window's resolution (not half), **sharpened** in the upscale (against the four texels' mean, held to
their range: no halo); the image history while moving is **6 frames** (not 16), as there is no noise to
average, so nothing smears.

**D8. Traced lighting stays a setting**: one or two bounces, as before, for anyone who wants the richer,
noisier light at its cost.

**D9. Every setting in the island's menu.** The Esc menu's **Graphics** opens a panel: preset, resolution
(50/75/100%), lighting (simple, traced 1 or 2 bounces), reflections (screen space, traced), shadows (soft,
hard), ambient occlusion, bloom, light shafts, depth of field, motion blur. A click cycles one; a change
makes the preset **Custom**; all of it is kept in `settings.json`.

## Milestones

| | What | Proven by |
|---|---|---|
| **P0** | Measure: the island at a first-person pose (`--look`), quality, and each D3-D5 lever alone, in one batch | a table in this plan |
| **P1** | Engine: the render scale (D2) and passes on and off by name (D4); unit tests for both | `ctest --preset dev`; the gallery at 0.5 |
| **P2** | The performance preset (D1, D3, D4); the gallery's panel and `--preset`; the island's menus, flag and default (D6) | 60 fps on the laptop in the island at its default window (the user's check); the look (the user's check) |
| **P3** | The optimizations (D5) that hold up, one commit each | before/after bench on the laptop; `projv_imgdiff` on quality; the user's run on the desktop |

## Open questions

None. Answered 2026-11-03: the render scale is **half the window** (not a fixed height); the island
**picks performance on an integrated GPU** and remembers a choice made in its menu; performance keeps
reflections **in screen space**.

## As built

**P0** (2026-11-03): the tables above.

**P1** (2026-11-03). `ConstructedRenderer::renderScale` and `graphics::renderTargetSize` (D2): the driver
sizes window-relative targets from the window times the scale. `graphics::setPassEnabled(renderer, name, on)`
(D4) over the passes' existing `enabled` and `name`. Tests: `tests/unit/test_render_passes.cpp`.

**P2** (2026-11-03). `restir_pt::Preset`, `settingsFor`, `presetName`; `upload` applies a preset's render
scale and passes, and hands the rest the renderer's own size. New uniform `restirPreset`: x screen-space
reflections (`reflect.frag`'s march: 24 steps from 3 finest voxels, each 1.2x the last; a hit in the
mirror's own plane is not one -- grazing rays over water otherwise "hit" the water just beyond them), y the spatial
neighbours (`spatial.frag`; a pixel with no history takes twice as many, at most 10, as before), z the camera
hit's sun coarse past that many voxels (`restir_trace.sc`, when `RESTIR_SHADOW_LOD` is 0). `display.frag`
upscales bilinearly when its input is smaller than the window, and is unchanged at full size.

Two departures from the decisions, from looking at it:

- **The visibility ray stays** (D3 dropped it): without it, light from the neighbours' paths leaked into
  shadow -- a shaded cliff came out nearly as bright as a sunlit one. 1.4 ms at 720p.
- **Fewer passes are skipped** (D4): the depth of field and motion blur are links of the post chain
  (`resolve -> dof -> motion_blur -> display`), and the SSAO pass writes the depth `ao_apply` filters by;
  they run with their effect off. Skipped: the bloom's nine passes and the shafts.

| 1280 x 720, the laptop, the island's spawn | frame | `gbuffer` | `sample` | `spatial` |
|---|---|---|---|---|
| quality | 40.2 | 6.9 | 13.8 | 8.3 |
| performance | **8.5** | 2.3 | 2.1 | 1.9 |

The island demo (D6): `settings.json` beside the program; with none, the first 3 s of play (after 1 s to
settle) are timed and frames over 25 ms on average pick performance -- bgfx does not say whether a GPU is
integrated, so the island asks how fast it is instead (this laptop: performance). The Esc menu's
**Graphics** button switches and keeps the choice; `--graphics quality|performance` sets one run's and
keeps nothing; captures and the self-test draw quality. The gallery: `--preset`, and the panel's preset.

**D7-D9** (2026-11-03). `Settings::simpleLighting` and `sharpen`; `restirPreset.w` (simple lighting,
`composite.frag`), the new uniform `restirUpscale` (`display.frag`'s sharpening). Under simple lighting
`upload` skips `surface`, `sample`, `spatial`, `gi_accumulate` and the four a-trous passes. Performance at
1280 x 720, the island's spawn, the laptop: **6.9 ms** (`gbuffer` 3.9 of it, at 0.75). The island's panel:
`ui/graphics.rml` (model `graphics`: rows of label, value, key); `settings.json` keeps
`{"graphics": "custom", "base": ..., "settings": {...}}` for custom ones; Esc in the panel goes back to the
menu. `ISLAND_MENU=pause|graphics` opens one for a capture.

Shaded ground under simple lighting is brighter and flatter than quality's: it has no occlusion but the
SSAO's. A look for the user to judge.

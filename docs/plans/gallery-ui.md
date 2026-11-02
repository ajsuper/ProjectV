# The renderer gallery, driven from the UI

Status: **in progress**: approved 2026-11-01 (the four questions below, answered by the user); G1 built 2026-11-01, G2 next. Builds on
[ui.md](ui.md) U1 (the dev layer).

Goal: `examples/30-renderers` shows three renderers -- **restir-pt**, **pt-reference** (the ground
truth) and **fast** -- switched, and each one's settings changed, from a panel in the dev layer.

## Why: where it stands (2026-11-01)

- Five renderers in the registry (pt-reference, fast, reconstruction, restir-pt, voxel); five more
  folders in `renderers/` that nothing loads any more (face, radiance-cascade, reprojection,
  world-cascade, world-face-cascade).
- One renderer a run, chosen by `--renderer`. Switching means quitting.
- Settings are about twenty keys, all restir-pt's (G, J, P, V, B, E, K, N, M, O, L, U, I) plus Q and T
  for renderers that are going. pt-reference's bounces and fast's AO are compile-time constants.
- The gallery polls GLFW itself, so the engine's UI (which routes the engine's `Input`) cannot sit on it.

## Decisions

**D1. Three renderers; the gallery-only rest are deleted.** `renderers/reconstruction` and the five
orphaned folders go. The voxel renderer leaves the gallery but stays in `examples/shared/voxelRenderer`,
where the demos draw with it (`MAYHEM_RENDERER=voxel` is how to look at it now). *Why:* the user's list.

**D2. Built on first use, kept.** A renderer is constructed the first time it is picked and stays
resident; switching back is instant and keeps its history. A switch counts as a camera move for the
renderer switched to, so its accumulation and reprojection start over from the current view.
*Why:* the user's choice; nothing in the engine destroys a constructed renderer yet, so rebuilding
would leak.

**D3. Settings in the UI only.** The per-renderer keys go. The keyboard keeps the camera (WASD, R/F,
Shift, mouse look, Esc/click for the cursor), H (re-frame), C (print the pose) and F1. The environment
variables (`RESTIR_*`, `GALLERY_*`) stay: they set the starting values, which unattended runs need.
*Why:* the user's choice.

**D4. The panel shows at start; unattended runs hide it.** `--bench` and `--capture` start with the dev
layer hidden and the input ignored, so measurements and captures are unchanged. *Why:* the user's choice;
the gallery is a tool and the panel drives it.

**D5. The gallery moves onto the engine's platform layer.** `graphics::installPlatform` and
`ui::installUi`; the camera reads `Input` (mouse look from `cursorDelta`, the sun from `scroll`), so a
click or a wheel over the panel never moves the camera or the sun.

**D6. Each renderer gets real settings.**

| Renderer | Settings |
|---|---|
| all | the renderer; the sun's angle (the wheel's, as a slider); re-frame, print pose |
| pt-reference | bounces (1-6); restart accumulation; frames accumulated |
| fast | AO radius, intensity and power; the denoise blur on/off -- shader constants made one uniform, `fastSettings`, defaults unchanged |
| restir-pt | the view (G's); reuse, and its temporal / spatial / visibility halves; the final render (P) and its parts (vignette, grade, exposure, DOF, bloom, motion blur, fog, aerial haze, shafts); soft shadow, blurred normals and their width, SSAO and far AO, path bounces, validation. GI stride stays `--restir-gi` (it sizes targets at construction) |

## Milestones

| | What | Proven by |
|---|---|---|
| **G1** | Platform + UI, the three renderers built on first use, the panel with the switch; deletions | every renderer draws; `--bench`, `--capture` unchanged (captures bit-identical to before at the baseline view) |
| **G2** | Each renderer's settings (D6); the keys removed; README and docs | the user drives each by hand |

## As built

**G1** (2026-11-01).
- Three renderers, in the user's order: restir-pt (now the default), pt-reference, fast. Deleted:
  `renderers/reconstruction` (tracked); the five orphaned folders, which turned out to hold only
  ignored build leftovers; and `sharedShaders/pjv_cascade*.sc`, which only the cascade renderers
  read. The voxel renderer is no longer staged into the gallery.
- `Gallery` (in `world.ctx()`) holds the registry, the renderers built so far and the active one.
  - A renderer is built on first use, as renderer slot `index + 1`, and kept.
  - A switch sets `switched`, which the frame treats as a camera move. restir-pt also treats it as
    its first frame, so every history starts over from the current view.
  - The window title names the active renderer.
- The gallery is on `graphics::installPlatform` and `ui::installUi`. The camera reads `Input`: mouse
  look from `cursorDelta`, the sun from `scroll`, and WASD/R/F/Shift/H/C as keys. Esc frees the
  cursor and a click captures it, so a click or a wheel over the panel never reaches the camera.
- The **Gallery** panel has the renderer (radio buttons) and its one-line description, the sun (a
  slider over the wheel's angle), and the Re-frame (H) and Print pose (C) buttons. It is shown at
  start, and hidden in `--bench` and `--capture`.
- `GALLERY_SWITCH_AT=n:id,...` switches renderer at those frames, as the panel does: switching,
  unattended.
- Checked:
  - Captures at the baseline view are bit-identical to before for all three.
  - A run switching restir-pt -> pt-reference -> fast -> restir-pt -> fast built each renderer once,
    and its last frame is bit-identical to a run that only drew fast.
- restir-pt's keys still work (now through `Input`) until G2 replaces them with the panel.

## Open questions

None open.

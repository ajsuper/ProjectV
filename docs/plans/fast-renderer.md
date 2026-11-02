# The fast renderer, made fast

Status: **built** 2026-11-01 (F1); approved the same day (the two questions below, answered by the user).
`examples/30-renderers/renderers/fast`, the gallery's cheapest renderer ([gallery-ui.md](gallery-ui.md)).

## Why: where it stands (2026-11-01)

At 1920x1080, the gallery's `--bench` with `GALLERY_SIZE=1920x1080`:

| fast | ms | |
|---|---:|---|
| `gbuffer_shade` | 2.57 | a camera ray and a sun shadow ray, **every window pixel** |
| `ssao` | 1.14 | 12 samples, each a 16-byte world-position read |
| `combine_blur` | 0.73 | 5x5 bilateral, two G-buffer reads per tap |
| **frame** | **4.55** | restir-pt at 1 bounce: 7.25 |

restir-pt's whole G-buffer is 1.45 ms, because it traces half resolution each way and rebuilds the
window from voxel face identity. fast also claims TAA and has none: no jitter, no temporal pass, so its
edges alias. Its AO noise came in stripes (a clamped sampler), fixed separately (de94d3e).

## Decisions

**D1. Trace at half resolution, rebuild the window by face identity.** The G-buffer, the SSAO and the
blur run at ½ × ½. The window is rebuilt by the voxel renderer's reconstruction, the one restir-pt
uses: for each window pixel, the voxels its 3x3 render texels saw, tested against the pixel's own ray,
antialiased by coverage. *Why:* the user's choice; a quarter of the rays, and a voxel face is a flat quad
that one sample describes, so the window keeps its edges.

**D2. TAA.** The camera rays jitter over 16 Halton phases inside each render texel, and a temporal
resolve accumulates them with the engine's previous-frame camera (64 frames still, 16 moving, clamped
to the 3x3 neighbourhood). *Why:* the user's choice; it is what the description always promised.

**D3. Copied, not shared.** The reconstruction and the resolve are copied from restir-pt into fast's
folder, as restir-pt copied them from the voxel renderer. The resolve drops restir-pt's water and mirror
histories, which fast has no material for. *Why:* the gallery's rule that no renderer borrows another's
files, so each can be read alone. Promoting the reconstruction into the engine's shader library is the
real fix for the copies; not in this plan.

**D4. Motion from the camera alone.** fast's motion vectors reproject by the previous camera, not by
each chunk's previous pose (restir-pt's `prevPoses`). The gallery's scenes do not move; a moving chunk
would ghost under fast's TAA. *Why:* the cheap renderer stays cheap; recorded as its limit.

**D5. The panel gains TAA on/off and the reconstruction on/off**, beside the AO settings: the A/Bs for
what each buys.

## Milestones

| | What | Proven by |
|---|---|---|
| **F1** | Half-res G-buffer, SSAO, blur; reconstruction; TAA; the panel's two switches | `--bench` at 1920x1080 (target: well under restir-pt, and under its old 4.55 ms); captures by eye |

## As built

**F1** (2026-11-01).
- `render.json`: `gbuffer_shade`, `ssao`, `combine_blur` at ½ × ½, then `reconstruct`, `resolve` and
  `display` at window size.
  - The G-buffer is in restir-pt's slot layout: gPos (w the distance, -1 sky), gNormal, gAlbedo,
    gDirect (a the hit), gFace, gRot, gMotion.
  - The SSAO and the blur take theirs as single textures (`resourceTexturesIDs`).
- `gbuffer_shade` traces the camera ray and the shadow ray from one march call site, as restir-pt's
  does. It used two, each a full inlined march.
- Two more bugs in the old pipeline went with it; both made the AO wrong everywhere, not only at the
  frame's edges:
  - `gPos.w` held the hit chunk's header index, which the SSAO read as the distance from the camera.
  - The SSAO decoded the normal `* 2 - 1`, as if packed in bytes, from a float target.
- Measured, `GALLERY_SIZE=1920x1080 --bench` on SanMiguel:

  | | before | after |
  |---|---:|---:|
  | gbuffer_shade | 2.57 | 1.20 |
  | ssao | 1.14 | 0.21 |
  | combine_blur | 0.73 | 0.20 |
  | reconstruct | -- | 0.90 |
  | resolve | -- | 0.12 |
  | **frame** | **4.55** | **2.79** (restir-pt at 1 bounce: 7.25) |

- By eye, against pt-reference at SanMiguel's default view: the old image's black alcove and brown
  floor streak were the AO bugs. The ground truth has neither, and neither does the new image. In the
  material room, the stripes and the misplaced darkness are gone, contact occlusion sits in the
  corners and under the cubes, and the edges are clean.
- Not measured: the TAA's ghosting under fast motion. That needs the user's eye.

## Open questions

None open.

# ReSTIR PT: paths with materials, reused

Proposed 2026-10-22. Status: **proposal**.

The gallery's `restir` renderer is ReSTIR GI: one diffuse bounce reused by reconnection, Lambertian
everywhere. Two things it cannot do:

- **Materials.** Glossiness and metallic are in every palette (`decodeMaterial`) and ignored. A glossy
  floor, a metal panel, a mirror all shade as chalk.
- **Stale light.** A reservoir caches the light its path saw (`Lo`). The renderer resets every history
  when the *sun* moves (done 2026-10-22, see `examples/30-renderers/renderers/restir/README.md`), but any
  other change -- an explosion, a door, a light switched on in a game -- is reused, and spread by the
  spatial feedback, until the reservoir happens to be replaced.

ReSTIR PT (Lin et al. 2022, *Generalized Resampled Importance Sampling*) answers both: it reuses whole
paths through arbitrary BSDFs, with a shift that re-traces the path's view-dependent start and reconnects
only where reconnection is safe.

## Design

### Material model -- one, shared

`include/pjv_bsdf.sc`, hoisted from the scene editor's Render-tab path tracer (`renderShaders/
path_trace.frag`), which already defines it:

| Term | Value |
|---|---|
| roughness | `alpha = max(1 - glossiness, 0.03)²` (GGX) |
| diffuse | `albedo × (1 - metallic)`, Lambert, × `(1 - F)` |
| specular | GGX D, Smith G, Schlick F; `F0 = mix(0.04 × glossiness, albedo, metallic)` |

Rule kept from the editor: glossiness 0 and metallic 0 -- every palette written before materials -- is
*exactly* Lambert (dielectric F0 is scaled by glossiness). Sampling: VNDF for the specular lobe, cosine
for the diffuse, the lobe picked by estimated weight. The editor's path tracer moves onto the include, so
the editor and the gallery cannot disagree.

### Paths

From the primary hit x1: up to **3 bounces**, BSDF-sampled, with next-event estimation to the sun at every
vertex (MIS against BSDF sampling: the sun is a disc, a glossy lobe can hit it). Emission at every vertex.
Transparent voxels stay solid (`raySceneIntersectFrom`), as now.

### Reservoir: a path, not a point

| Field | Why |
|---|---|
| seed | **random replay**: the prefix is re-traced from it at the shifted pixel |
| k | the reconnection vertex's index (0: none, replay all) |
| x_k, n_k, material of x_k | reconnect to it from a new x_{k-1} |
| ω_k (direction out of x_k), L_k (light arriving along it, the suffix's) | re-evaluate x_k's BSDF for the new incoming direction |
| target, W, M | resampling |

**Hybrid shift.** Replay the prefix x1..x_{k-1} with the stored seed; reconnect to x_k when both x_{k-1}
and x_k are rough (`alpha > 0.2`) and far enough apart (a few voxels): the path's first rough-rough pair.
A diffuse wall seen in a mirror: the mirror bounce is replayed, the wall reconnected. All-glossy paths
(mirror to mirror) are replayed whole. Jacobian: the reconnection term (as now) times the replay's
(1 for replay with the same random numbers).

**Target**: luminance of the full path contribution, primary BSDF included -- view-dependent, so the
shift re-evaluates it at the new pixel.

**MIS**: pairwise MIS (Bitterli's, as the paper uses) for spatial reuse; the temporal pair likewise.
Feedback of the spatial result into the temporal history, as `restir` does since 2026-10-22.

### Stale light: validation

Each frame a quarter of the pixels (rotating) re-trace their reservoir's path suffix from x_k. If the light
it now carries differs by more than a threshold, the cached L_k is replaced and M clamped to 1 (Kajiya's
validation). Any change of light -- told or not -- is gone from the reservoirs within four frames. The sun
reset stays (it is free and immediate).

### Denoising

Diffuse and specular separately, each demodulated (diffuse by diffuse albedo; specular by its
pre-integrated directional albedo, a small LUT). Diffuse: as now (12-frame accumulation, à-trous).
Specular: history length from roughness (a mirror keeps ~2 frames, rough ~12), reprojected at the
reflected point's virtual depth for glossy surfaces; à-trous radius scaled by roughness.

### Direct light

The sun through the BSDF, highlight included: `shadow_soft` computes diffuse + specular sun with the soft
visibility. The G-buffer carries glossiness and metallic (it is at the nine-input limit for the composite:
packed into `gDirect.a` as `glossiness × 256 + metallic` in an RGBA32F `gDirect`).

## Reference

A gallery renderer `pt-reference`: a plain accumulating path tracer on `pjv_bsdf.sc`, same sun, sky, path
length and display. It converges slowly and exactly; every milestone compares against it (linear,
downsampled, as `gi_eval.sh` did for the lag work) on a material test scene -- the test room with a glossy
floor, a metal wall and a mirror panel.

## Milestones

| # | What | Done when |
|---|---|---|
| T0 | `pjv_bsdf.sc`; the editor's path tracer on it (pixel-identical); `pt-reference`; the material test scene | the editor's Render tab unchanged; `pt-reference` converges on the test scene |
| T1 | `restir-pt` (a copy of `restir` to start): G-buffer materials; sun through the BSDF; paths with BSDF sampling + NEE; reservoirs as paths; reconnection-only shift | diffuse scenes match `restir`; glossy direct highlights match the reference |
| T2 | Hybrid shift (random replay) and pairwise MIS | glossy-floor reflections of diffuse walls converge to the reference |
| T3 | Validation | an unannounced light change settles within ~4 frames of reservoir age |
| T4 | Split, demodulated denoising; specular reprojection | reflections stable under camera motion, no ghosting |
| T5 | Retire `restir` (or keep it as the cheap option) | `restir-pt` at least as good on diffuse scenes, cost measured |

## Open questions

1. **Resolution.** `restir` traces GI for 1 pixel in 16. Glossy reflections at that density are blocky;
   1 in 4 (`--restir-gi 2` today) is likely the floor for specular. Cost to be measured in T1.
2. **Replace or sit beside.** Proposed: `restir-pt` beside `restir` until T5.
3. **Transparency** (glass refraction, rough transmission) is out of scope here; it would follow T4.

# Colour convention: one meaning for a stored colour, one transform to the screen

Proposed 2026-10-22. Status: **K0-K2 landed 2026-10-22** (with restir's post chain); K3-K5 open.

> Measured on Teardown levels (Tuxedo Labs) converted from a local install with `mesh_voxelizer
> --td-*`. They are not in the repo and may not be redistributed, so the numbers here are a record,
> not something a checkout can rerun. Credit: `SOURCES.md`.

Teardown levels converted by `mesh_voxelizer --td-*` look too high-contrast in `restir`. In every other
renderer they look washed out. The cause is not the converter. ProjectV never says what a palette colour
*means*, so each renderer guesses, and each display pass grades the result differently.

## What happens today

```
 importer            compose.json "color"          GPU decodeMaterial         renderer               display pass
 ─────────────       ─────────────────────         ──────────────────         ────────               ────────────
 sRGB bytes  ──x4──▶ R10G10B10 (undeclared)  ──/1023──▶ albedo  ──┬─ as linear (all but restir) ─▶ ACES, ^(1/2.2)   → washed out
                                                                  └─ ^2.2 (restir, key C)      ─▶ ACES, ^(1/2.2),  → too contrasty
                                                                                                  contrast 1.12, saturation 1.25
```

| Stage | Today | Problem |
|---|---|---|
| Storage | `packColor`: 8-bit × 4 into 10 bits. Every importer writes **sRGB-encoded** bytes: textures (mesh), block textures (Minecraft), `.vox` palettes (Teardown), colour pickers and hex literals (editor, examples). | Not documented. 255 → 1020, so white is 0.997. |
| Decode | `decodeMaterial` (`include/pjv_utils_DDA.sc:979`): `/1023`, nothing else. | sRGB values used as linear albedo: mid-tones lifted (sRGB 0.5 is read as 0.5, but it is really 0.21 linear), shadows grey, saturation lost. **This is the washed-out look.** |
| Restir's fix | `restir_trace.sc:82`: `pow(albedo, 2.2)` behind key C, on by default. | Correct in spirit, but only in one renderer, and only for camera hits. Emission is not decoded. |
| Display | About ten hand-written passes (`fast`, `taa`, `reconstruction`, `tree64`, `restir`, `shared/voxelRenderer`, `nightRenderer`, `basicRenderer`, advanced, editor ×2). ACES (Narkowicz fit) plus `pow(1/2.2)`. `tree64` uses `1/3.2`. The voxel renderer and restir add contrast ×1.12 and saturation ×1.25. | ACES already adds toe contrast and over-saturates. The grade on top of that is the **too contrasty** look, now that restir's albedo is properly darker. A grade that was tuned to compensate for washed-out input is applied to input that is no longer washed out. |
| Lighting scale | Each renderer has its own sun and sky intensity and its own exposure. Restir dims the sky to 0.35. | No reference: nothing says what a lit albedo of 0.5 should come out as on screen. |

## The convention

**C1. A stored colour is sRGB-encoded, 10 bits per channel.** This applies to `"color"` and `"emission"` in
compose.json, `Material::packedColor` and `packedEmission`, and the palette texel. It is what every file on
disk already holds, so **no data migration is needed**. Ten bits of sRGB beat ten bits of linear near
black, where the eye needs the precision: the first step is 1/13 200 linear instead of 1/1023.

- `packColor` rounds `c * 1023 / 255` (white = 1023); `unpackColor` inverts it.
- `docs/data_structures/compose_data_structure.md` documents the encoding.
- Emissive *strength* stays linear and separate, as it is now.

**C2. Shading is linear, and decoding happens once, in the engine.** `decodeMaterial` applies the sRGB EOTF
(the piecewise curve, not 2.2) to the albedo and emission colours. Every renderer then receives linear
albedo with no changes of its own. Restir's `pow(…, 2.2)` and the C key go.
- Cost: one `pow` per material fetch (not yet measured). If it shows up in profiles, the palette upload
  can instead write a linear RGBA16F side texture, a CPU-side change with the same contract.
- On the CPU, a `utils::linearAlbedo(const Material&)` would do the same for code that shades colours outside
  a shader. Not added: nothing on the CPU shades a palette colour yet (the Teardown preview works on the
  `.vox` bytes, and the editor's swatches show the stored colour, which is right as it is).

**C3. Averaging and quantizing happen in linear (or Oklab), never on the encoded bytes.** The Teardown
writer already does this. The mesh voxelizer's median cut and Minecraft's `averageTexture` do not: they
average sRGB bytes, which darkens mixtures slightly. That is a small effect, fixed alongside.

**C4. Scene-referred calibration.** A Lambertian surface facing the sun, under the renderer's default sun and
sky at exposure 1, receives an irradiance whose outgoing radiance equals its albedo:
`L = albedo × 1.0`. Each renderer scales its own sun and sky constants to meet this, and keeps its own ratio
of sun to sky. This makes "lit at noon" mean the same thing in every renderer.

**C5. One display transform, in one include.** `include/pjv_color.sc`:

```glsl
vec3 pjvSrgbToLinear(vec3 c);              // the piecewise EOTF (also used by decodeMaterial)
vec3 pjvLinearToSrgb(vec3 c);              // the piecewise OETF, replacing every pow(x, 1/2.2)
vec3 pjvTonemapNeutral(vec3 c);            // Khronos PBR Neutral
vec3 pjvDisplay(vec3 hdr, float exposure); // exposure -> tonemap -> OETF -> dither
```

The tone curve is the **identity up to 0.76, then Khronos PBR Neutral's highlight shoulder** -- not PBR
Neutral whole. Its toe (`x - 6.25x²` below 0.08) and its 0.04 offset assume a 4% Fresnel layer on every
surface; on Lambert-only shading they crush dark interiors (linear 0.016 comes out 0.0016), which is the
opposite of what this is for. (The first draft of this plan named PBR Neutral whole; corrected before
anything landed.) With C4, a sunlit surface therefore shows **its stored colour on screen**, so a Teardown
palette looks like the Teardown palette, and shadows keep their level. ACES pulls the same surface
around: crushed toe, extra contrast, hue shifts in saturated reds and blues.

A grade (contrast, saturation, vignette) stays allowed, but as an explicit, named *look* on top of
`pjvDisplay`, defaulting to the identity. A renderer that wants the voxel renderer's punch opts in, and
the default is faithful. `pjvGrade(c, contrast, saturation, whiteBalance)` and `pjvVignette(uv, aspect,
strength)` are the shared pieces: linear light, before the curve. Contrast is a **power about middle grey**,
not a stretch about display 0.5 -- the old `(c - 0.5) * 1.12 + 0.5` sent every display value below 0.054
(linear 0.002 after ACES) to zero.

## Acceptance test

A colour checker of 24 patches (Macbeth sRGB values) as one component, lit head-on by the sun, with the sky
off. Every renderer in the gallery renders it with `--renderer X --scene tests/data/colorchecker`. The
patches below about 0.76 linear must reproduce their stored sRGB within ΔE2000 < 3, and the white patch
must not clip. One CPU unit test covers `packColor`/`unpackColor` and the EOTF/OETF round trip. One
headless check reads back the display pass for the checker in `restir` and `voxel`.

## Steps

| # | Step | Touches | Visible change |
|---|---|---|---|
| K0 ✓ | Write the convention into `compose_data_structure.md`. Add `pjv_color.sc`. Fix `packColor` rounding. Add the unit test. | engine, docs | none (white 1020 → 1023) |
| K1 ✓ | Decode in `decodeMaterial` (C2). Delete restir's decode and the C key. | engine shader, restir | every renderer except restir gets darker and more saturated, as intended |
| K2 ✓ | Move every display pass onto `pjvDisplay`, and keep the existing grades as looks on `pjvGrade`. Fix `tree64`'s 1/3.2. | ~10 display shaders | restir's excess contrast is gone, and the renderers agree |
| K3 | Calibrate sun and sky per renderer (C4) against the colour checker. Drop restir's 0.35 sky hack if C4 makes it unnecessary. | renderer constants | exposure levels match across renderers |
| K4 | Linear averaging in the mesh and Minecraft importers (C3). | voxelizer | slightly brighter blended colours |
| K5 | Look over the hand-picked colours in the examples (hex literals in `70`/`80`/`90`, the terrain generator, the explosion maps). They were chosen by eye under the washed-out pipeline, and after K1 they will read darker and stronger. Most were authored as sRGB intent, so most will simply look *as meant*; retune those that do not. | examples | per-example |

K1 and K2 should land together, or at least back to back. K1 alone makes the graded renderers look harsh
for the same reason restir does now.

## Not in scope

- Wide gamut and HDR output. The backbuffer stays RGBA8 non-sRGB, with the OETF applied in the shader.
  `pjvDisplay` is the one place a later HDR path would branch.
- Per-voxel colour beyond the palette.

## Delivery log

**K0-K2, 2026-10-22.**
- `include/pjv_color.sc`: `pjvSrgbToLinear`/`pjvLinearToSrgb` (piecewise), `pjvTonemapNeutral`, `pjvDither`,
  `pjvDisplay`, `pjvGrade`, `pjvVignette`. Included by `pjv_utils_DDA.sc`, installed with it.
- `decodeMaterial` decodes albedo and emission colour; every renderer's palette reads go through it.
- `packColor` widens 255 to 1023 (was 1020) and `unpackColor` rounds; `tests/unit/test_color.cpp`.
- Display passes on `pjvDisplay`: gallery `fast`, `taa`, `reconstruction`, `tree64` (its 1/3.2 gone),
  `restir`; `shared/basicRenderer`; `shared/voxelRenderer` and `nightRenderer` (their grade kept as
  `pjvGrade`, contrast 1.08 / saturation 1.10, was 1.12 / 1.25 display-space); the advanced renderer (1.08 /
  1.15, was 1.1 / 1.3); the scene editor's preview (now encodes: an unlit albedo shows its stored colour; the
  backdrop greys decoded, as they were picked in display terms) and Render tab (neutral curve, exact sRGB;
  its user grade unchanged).
- restir: the palette decode and the C key gone; a post chain (P: vignette V, grade B, auto-exposure E) --
  see `examples/30-renderers/renderers/restir/README.md` (since 2026-10-22 `restir-pt/`, which replaced it). Auto-exposure key 0.08 at 80% strength, measured:
  sunlit Lee Chemicals meters 0.064 and moves under 0.3 stops; the test room's back wall meters 0.007 and
  is lifted about 2.8 stops.
- Checked: unit suite and layering pass; gallery captures of Lee Chemicals in restir, voxel, fast,
  reconstruction and taa. Not looked at on screen: the basic renderer's examples, Explosion's night renderer,
  the advanced renderer, the scene editor.
- Seen, not fixed: `reconstruction` is brighter than the rest (mean 150 vs restir's 103 on the same view)
  -- its sun and sky are uncalibrated (K3). `tree64` renders black on Lee Chemicals, before this change too.

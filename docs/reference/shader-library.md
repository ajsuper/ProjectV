# Shader library

[`shaders/include/`](../../shaders/include/pjv_engine.sc) is the engine's shader include root, on every
shader's path through `projv_compile_shaders` (installed as `<prefix>/include/projv/shaders`).
Mechanisms only: a file takes what it needs as arguments or reads an engine-owned uniform, and holds
no look ([plan](../plans/shader-library.md)). Its centre is
[`pjv_utils_DDA.sc`](../../shaders/include/pjv_utils_DDA.sc), the engine's GLSL (bgfx shaderc) for
marching rays through the voxel scene; its comments are the full reference, this is the map.

```glsl
#include <bgfx_shader.sh>
#include <pjv_utils_DDA.sc>        // on shaderc's include path via projv_compile_shaders
```

## A complete primary ray

```glsl
Ray ray;
ray.origin    = cameraPos.xyz;
ray.direction = rayStartDirection(v_texcoord0, windowRes.xy, cameraPos.xyz, normalize(cameraDir.xyz), 60.0);

RayQuery q = pjvPrimaryQuery(256u);          // max steps
pjvQueryWantMaterial(q);                     // have the march resolve the material

SceneHit sh = raySceneIntersect(ray, q);
if (sh.hit.rayT < 0.0 || sh.hit.foundBox.size < 0.0) { /* miss: sky */ }
VoxelMaterial m = sh.materialValid ? sh.material : fetchVoxelMaterialFromHit(sh.hit);
vec3 position   = ray.origin + ray.direction * sh.hit.rayT;   // only valid without refraction
```

## Companion includes

| File | Holds | Plan |
|---|---|---|
| [`shaders/include/pjv_math.sc`](../../shaders/include/pjv_math.sc) | `PJV_PI`, `PJV_TWO_PI`; `pjvBasisAround(n)` (tangent from +z, or +y near the poles); octahedral `pjvOctEncode`/`pjvOctDecode` in [-1, 1]². | [shader-library](../plans/shader-library.md) |
| [`shaders/include/pjv_random.sc`](../../shaders/include/pjv_random.sc) | `pjvPcg(state)` (one PCG step); `pjvRandomUnit` (32 bits, may round to 1) and `pjvRandomUnit24` ([0, 1) exactly); `pjvHashSeed(uvec3)`; `pjvHash13`/`pjvHash23`/`pjvValueNoise` (−1..1); `pjvInterleavedGradientNoise(pixel)`; `pjvBayer4(pixel)`; `pjvHalton(index, base)`. | [shader-library](../plans/shader-library.md) |
| [`shaders/include/pjv_sampling.sc`](../../shaders/include/pjv_sampling.sc) | Local-space (about +z) directions from `u` in [0, 1)²: `pjvSampleCosineHemisphere(u)`, `pjvSampleUniformCone(u, cosMax)`. The caller picks the basis: it is part of its noise pattern. | [shader-library](../plans/shader-library.md) |
| [`shaders/include/pjv_filter.sc`](../../shaders/include/pjv_filter.sc) | `pjvTexelTent(tapCentre, srcPos)`; `pjvCatmullRomTaps(uv, res)`: the five-tap bicubic's tap positions and weights, sampled by the caller. | [shader-library](../plans/shader-library.md) |
| [`shaders/include/pjv_engine.sc`](../../shaders/include/pjv_engine.sc) | Every engine-owned uniform, declared once: `passTargetRes`, `passInputRes[8]`, `pjvAnimTime`, `pjvMotionSets[48]`. Include it rather than declaring them. | [shader-library](../plans/shader-library.md) |
| [`shaders/include/pjv_color.sc`](../../shaders/include/pjv_color.sc) | The colour convention: `pjvSrgbToLinear`/`pjvLinearToSrgb`, `pjvTonemapNeutral`, `pjvDisplay` (exposure → tone curve → sRGB → dither), `pjvGrade`, `pjvVignette`. `decodeMaterial` uses it, so every renderer gets **linear** albedo from sRGB-stored palettes. | [color-convention](../plans/color-convention.md) |
| [`shaders/include/pjv_bsdf.sc`](../../shaders/include/pjv_bsdf.sc) | The surface model: a Lambertian diffuse lobe under a GGX specular lobe (`pjvBsdfEvaluate`, `pjvBsdfSample`, `pjvPowerHeuristic`, ...). A default material (glossiness 0, metallic 0) costs nothing and changes nothing. The editor's path tracer, `pt-reference` and `restir-pt` all shade through it. | [restir-pt](../plans/restir-pt.md) |

## Rules

- **One march call site per shader.** A second inlined `raySceneIntersect` / `raySceneIntersectFrom`
  call made every mode of a renderer about 2× slower. Choose the ray first, branch inside one call.
- **LOD is not a performance lever.** `startLOD`/`finishLOD`/`distanceToFinishLOD` tune VRAM and
  anti-aliasing, not frame time. A slow renderer is the wrong GPU/driver, target count or size, or
  CPU work. A ray that runs out of `maxRaySteps` reports a miss, so a fast black screen means
  too few steps.
- **Use the hit's own data.** `rayT`, `normal`, `voxelCoord`, `exitT` come straight from the march.
  Re-intersecting `foundBox` disagrees by ULPs and misclassifies boundary hits; recovering the voxel
  from the world position shades voxels with their neighbour's colour on moved chunks.
- **Build queries with a constructor.** A hand-made `RayQuery` fails the `magic` check, gets its
  newer fields clamped, and stops with `PJV_STOP_BAD_QUERY`.

## Checks

| Check | What |
|---|---|
| `ctest -R shader_library` | Every file compiles alone and together (SPIR-V, GLSL 440, Metal); no `uniform` outside `pjv_engine.sc`; every file-scope symbol `pjv`/`Pjv`/`PJV`. `pjv_utils_DDA.sc` is compiled but not scanned until it is split |
| `tools/shader_sizes.py [--save f] [--against f]` | Every compiled shader's size (renderer `.bin`s and the embedded engine shaders), or the change since a saved table |
| `projv_imgdiff before after [--diff out]` | Two captures (or two folders of them) within the tolerance: mean ≤ 0.1%, ≤ 0.1% of pixels off by more than 2/255; the largest difference reported |

## Bindings

| Stage | Sampler | Holds |
|---|---|---|
| 9 | `materialIDs` | Material bytes |
| 10 | `materialPalette` | Palettes, one entry per texel |
| 11 | `looseList` | Loose chunks + their BVH |
| 12 | `gridInfo` | Grids |
| 13 | `cellMap` | Grid cell → chunk |
| 14 | `tree64Data` | Every blob's voxels (its [tile table](formats/tile-table.md)) and envelopes. The name is from the tree64 it first held. |
| 15 | `headerData` | One header row per chunk |

All `USAMPLER2D`, read with `texelFetch`. Stages 0–8 are the pass's own inputs.

| Uniform | Set by | Holds |
|---|---|---|
| `tree64Dims`, `voxelTypeDims`, `paletteDims` | engine | Scene texture sizes |
| `pjvMotionSets[48]` | engine (`GPUData::animation`); declared in `pjv_engine.sc` | 16 motion sets × 3 vec4 |
| `pjvAnimTime` | engine; declared in `pjv_engine.sc` | `(seconds, 0, 0, 0)` |

## Entry points

| Function | Returns | Use |
|---|---|---|
| `rayStartDirection(uv, res, camPos, camDir, fovDeg)` | `vec3` | A camera ray for a pixel (matches `utils::rayDirectionThroughImage`) |
| `raySceneIntersect(ray, query)` | `SceneHit` | **The** scene query: transparency, refraction, animation per the query |
| `raySceneIntersectFrom(ray, query, tMin)` | `SceneIntersectData` | Nearest solid voxel beyond `tMin`, geometry only (probes) |
| `fetchVoxelMaterialFromHit(hit)` | `VoxelMaterial` | Material of a hit: two fetches, no descent |
| `fetchVoxelColorFromHit(hit)` | `vec3` | Albedo only |
| `fetchVoxelMaterialAtCoord(voxelCoord, headerIndex)`, `fetchVoxelColorAtCoord` | | By coordinate (re-descends) |
| `fetchVoxelColor(foundBox, headerIndex)` | `vec3` | Older form by box; prefer the `FromHit` ones |
| `pjvMotionSet(i)`, `pjvAnimSeconds()` | | The animation table |

## `RayQuery`

Constructors (one of these, always):

| Constructor | For |
|---|---|
| `pjvPrimaryQuery(maxSteps)` | Camera and reflection rays: nearest surface, whole ray |
| `pjvBounceQuery(maxSteps)` | Bounce rays (same today; named apart) |
| `pjvShadowQuery(maxSteps, maxDistance)` | Shadow rays: bounded, `PJV_Q_OCCLUSION_ONLY` |
| `pjvRayQueryRaw(maxSteps)` | All defaults |

Options (each sets its flag **and** its budget):

| Option | Flag | Effect |
|---|---|---|
| `pjvQueryWantMaterial(q)` | `PJV_Q_WANT_MATERIAL` | Fill `SceneHit::material` |
| `pjvQueryTransparency(q, maxLayers, seed)` | `PJV_Q_TRANSPARENCY` | See through `transparency > 0`; accumulate `transmittance`, `emission` |
| `pjvQueryRefraction(q, maxSegments)` | `PJV_Q_REFRACTION` | Bend at interfaces with `ior != 1` |
| `pjvQueryAnimation(q, resolveDistance)` | `PJV_Q_ANIMATION` | Resolve animated voxels within that distance |
| `pjvQueryAnimationDebugSolid(q)` | `PJV_Q_ANIM_DEBUG_SOLID` | Draw envelopes solid (diagnostic) |
| `pjvQueryLinearLOD(q, finishLOD, rampVoxels)` | | Coarsen linearly with distance |
| `pjvQueryLODFootprint(q, crossoverVoxels, coarsest)` | | Coarsen by pixel footprint |

Fields: `flags`, `startLOD`, `finishLOD`, `distanceToFinishLOD`, `maxRaySteps`,
`maxTransparentLayers`, `maxRefractionSegments`, `maxDistance`, `animResolveDistance`, `seed`,
`minTransmittance` (default 0.002).

## Results

`SceneIntersectData` (the geometry of one hit):

| Field | Meaning |
|---|---|
| `rayT` | Distance along the ray; −1 on a miss |
| `normal` | Outward normal of the entered face; zero if the ray started inside |
| `voxelCoord` | The cell, in the chunk's voxel space. Use this for lookups. |
| `exitT` | Where the ray leaves the cell |
| `headerIndex` | The chunk (its header row) |
| `foundBox` | The voxel's world box; `size < 0` on a miss |
| `steps` | March steps taken |

`SceneHit` (what `raySceneIntersect` returns): `hit`, `material` + `materialValid`, `transmittance`
(multiply what is behind by it), `emission` (add it), `layers`, `segments`, `finalRay` +
`pathLength` (the bent ray and the total distance under refraction), `stopReason` (`PJV_STOP_*`),
and diagnostics `diagBendFlags`, `diagBendIor`, `diagResolveMiss`.

| `stopReason` | |
|---|---|
| `PJV_STOP_OPAQUE` | Reached an opaque surface |
| `PJV_STOP_NO_GEOMETRY` | Nothing more along the ray |
| `PJV_STOP_LAYER_BUDGET`, `PJV_STOP_SEGMENTS`, `PJV_STOP_ITERATIONS`, `PJV_STOP_STEPS` | A budget ran out |
| `PJV_STOP_ABSORBED` | Transmittance under `minTransmittance` |
| `PJV_STOP_TMIN_STALL` | The resume floor did not advance |
| `PJV_STOP_BAD_QUERY` | Not built by a constructor |

## `VoxelMaterial`

The decoded palette entry ([Materials](../concepts/materials.md)): `albedo`, `emission` (already
× strength: radiance), `glossiness` (0 rough, 1 mirror), `metallic`, `transparency` (0 opaque), `ior`
(1 = none), `transmission`, `flags`. `pjvMaterialIsAnimated(m)` and `pjvMaterialIsAdvected(m)` read
the flags.

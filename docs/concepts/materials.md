# Materials

Every component owns a **palette** of up to 256 `Material` entries. Voxels store a slot in it, so
recolouring is a palette edit, not a geometry rewrite. Headers:
[`data_structures/scene.h`](../../include/data_structures/scene.h) (`Material` and its packing),
[`utils/material.h`](../../include/utils/material.h) (palette editing).

```text
voxel ──byte──▶ slot 3 ──▶ component.materialPalette[3] ──▶ Material { 4 GPU words, name, physics }
                                                                         │              │
                                       renderer: one RGBA32U texel ◀─────┘              └─▶ physics material
                                                                                            library ("stone")
```

## The four words

One entry is exactly one RGBA32U texel, so one `texelFetch` reads all of it.

| Word | Field | Layout | Accessors |
|---|---|---|---|
| 0 | `packedColor` | albedo, R10 G10 B10 | `packRGB10`, `unpackRGB10` |
| 1 | `packedEmission` | emission colour, R10 G10 B10. **0 = emit the albedo colour** | `materialEffectiveEmissionColor` |
| 2 | `packedSurface` | glossiness · metallic · transparency · IOR, a byte each | `packSurfaceWord`, `materialGlossiness`, ... |
| 3 | `packedExtra` | emissive strength · transmission · flags · reserved | `packExtraWord`, `materialEmissiveStrength`, ... |

Plus, off the GPU: `name` (editor-facing) and `physics` (a physics material name, below).

### The zero rule

**All-zero words 1–3 mean an opaque, non-emissive, fully rough surface**, exactly what a colour-only
palette always meant. That is why some fields are stored inverted:

| Stored | Not | Because 0 must mean |
|---|---|---|
| `glossiness` | roughness | fully rough |
| `transparency` | opacity | opaque |
| IOR as `1 + raw/128` | IOR | 1.0, no refraction |
| emissive strength, exponential: raw 0 = 0, raw 128 = 1.0, up to ~245 | linear | not emissive |

### Flags byte

| Bits | Meaning |
|---|---|
| 0 | `MATERIAL_FLAG_ANIMATED`: voxels of this material move |
| 1–4 | Motion set index (0–15): which animation field moves them ([`utils/animation.h`](../../include/utils/animation.h)) |
| 5–7 | Reserved |

Set them with `setMaterialAnimation(material, animated, set)`, which writes the flags lane only. A
whole-word `packExtraWord` would zero the emissive strength and the reserved byte with it.

## Building a palette

`internMaterial` dedupes and returns the slot:

```cpp
ComponentRecord& comp = scene.components[handle];
uint8_t stone = utils::internMaterial(scene, comp, "stone", packColor({120, 120, 125}));
uint8_t grass = utils::internMaterial(scene, comp, "grass", packColor({ 70, 160,  60}), "turf");
//                                                                        physics material ─┘
```

Thread-safe (locks `scene.materialPaletteMutex`) but not fast: dedupe locally when interning per voxel.

## Editing a palette

| Call | Changes | GPU update |
|---|---|---|
| `addMaterial(scene, comp, name, color)` | Appends a slot, no dedupe | `flushSceneUpdates` |
| `setMaterialColor(scene, comp, slot, color)` | One slot's albedo | `updatePaletteEntry` (one texel) |
| `setMaterialProperties(scene, comp, slot, w1, w2, w3)` | The other three words | `updatePaletteEntry` |
| `setMaterialName(...)`, `setMaterialPhysics(...)` | Name; physics material | none / physics rebuilds at its next step |
| `removeMaterial(scene, comp, slot, replacement)` | Removes a slot, **renumbers every voxel above it** | `flushSceneUpdates` |
| `countMaterialUsage`, `findMaterialChunks` | (queries) | |

Each bumps `ComponentRecord::paletteVersion`. Adding or removing an entry shifts later components'
offsets in the global palette, hence the full flush.

## On disk

In `compose.json`, per `data` entry, in slot order (reordering recolours the geometry).

```json
"palette": [
  { "name": "stone", "color": [480, 480, 500], "physics": "stone" },
  { "name": "lamp",  "color": [1023, 900, 600], "emissiveStrength": 4.0 },
  { "name": "glass", "color": [900, 950, 1000], "transparency": 0.9, "ior": 1.5 }
]
```

All keys: [compose.json → palette](../reference/formats/compose-json.md#palette).

## Physics materials

`Material::physics` names an entry in the **physics material library** (density, friction,
restitution, fracture). Many palette entries may share one.

```cpp
runtime::loadPhysicsMaterials(world, "physics_materials.json");
runtime::definePhysicsMaterial(world, "ice", {.density = 917, .friction = 0.02f});
```

Format: [physics_materials.json](../reference/formats/physics-materials-json.md). How bodies use
it: [Physics](physics.md#materials).

# compose.json

A folder's layout: which components it has, where they sit, what colours their voxels are. Parsed
by `utils::parseComposeJson`, loaded by `utils::loadComposeFromDisk`, written by
`utils::writeComposeJson` / `saveComposeToDisk` ([`utils/compose_io.h`](../../../include/utils/compose_io.h)).
In memory: `ComposeDoc` / `ComposeComponent` ([`data_structures/compose.h`](../../../include/data_structures/compose.h)).

```json
{
  "version": 1,
  "name": "World",
  "components": [
    { "type": "data",  "id": 1, "name": "Terrain", "source": "terrain.data",
      "position": [0, 0, 0], "rotation": [0, 0, 0, 1], "scale": 1.0, "mutability": "locked",
      "palette": [ { "name": "grass", "color": [280, 650, 240], "physics": "turf" } ] },
    { "type": "asset", "id": 2, "name": "Bird", "source": "../Assets/Bird",
      "position": [110, 240, 30], "rotation": [45, 5, 0] }
  ]
}
```

Strict JSON; `//` and `/* */` comments are accepted on read and not written back.

## Top level

| Key | Type | Required | Meaning |
|---|---|---|---|
| `version` | integer | **yes** | Must be `1`. Anything else: the file is refused. |
| `name` | string | no | For people. No runtime meaning. |
| `components` | array | **yes** | The entries below. May be empty. |
| `attachments` | object | no | Program data about the folder itself. See [Attachments](#attachments). |

## Components

| Key | Type | Default | Meaning |
|---|---|---|---|
| `type` | `"data"` \| `"asset"` | **required** | `data`: `source` is a `.data` file (a leaf). `asset`: `source` is a folder with its own `compose.json` (recurses). Anything else: the entry is skipped. |
| `source` | string | **required** | Path, relative to **this** file's folder. `../` is allowed. |
| `id` | unsigned integer | assigned | Persistent id, unique in this file. See [Ids](#ids). |
| `name` | string | file stem / folder name | Local name. Siblings with the same name get `_2`, `_3`, ... in memory. |
| `position` | `[x, y, z]` | `[0, 0, 0]` | Relative to the parent |
| `rotation` | 3 or 4 numbers | identity | See [Transforms](#transforms) |
| `scale` | number \| `[x, y, z]` | `1.0` | Must end up **uniform** on a `data` leaf |
| `mutability` | `"locked"` \| `"direct"` \| `"copy"` | `"locked"` | `data` only. See [Mutability](#mutability). |
| `palette` | array | `[]` | `data` only. See [Palette](#palette). |
| `attachments` | object | — | Program data about this entry. See [Attachments](#attachments). |

## Transforms

```text
world(component) = world(parent) · Translate(position) · Rotate(rotation) · Scale(scale)
```

| `rotation` length | Read as |
|---|---|
| 3 | Euler angles in **degrees**, `[x, y, z]`, converted with `glm::quat(radians(euler))` |
| 4 | A quaternion `[x, y, z, w]`. **What the writer always emits.** |
| other | Warning, identity |

- **Uniform scale only on `data` leaves.** If a leaf's accumulated world matrix has unequal axis
  scales, the leaf is rejected with an error and skipped. The 3-array form parses (and is written
  when the scale is not uniform), but only an equal-axes value is usable.
- An `asset` entry's transform is the parent transform for everything in its folder; transforms
  stack down the tree.

## Ids

- `id` is `ComponentRecord::localId`: unique among the entries of one file, kept across reloads,
  renames and reorders. **`entities.json` links by it.**
- Absent (files written before ids existed) or duplicated: the loader assigns a new one (a duplicate
  is warned about), and the next save writes it.
- Ids are local to the folder. A folder loaded twice has the same ids in both copies, each resolved
  against its own node.

## Palette

The colours and surface properties the `.data`'s material bytes index into, **in slot order**:
entry `i` is slot `i`. Reordering recolours the geometry. **255 usable slots** per component
(`MAX_PALETTE_ENTRIES`): slot 255 is `INVALID_MATERIAL`, "no slot". A 256th entry is folded into its
nearest colour on load, its voxels moved there, with a warning; entries past 256 are dropped.

```json
"palette": [
  { "name": "default", "color": [1023, 1023, 1023] },
  { "name": "lamp",  "color": [1023, 900, 600], "emission": [1023, 800, 400], "emissiveStrength": 4.0 },
  { "name": "glass", "color": [900, 950, 1000], "transparency": 0.9, "ior": 1.5, "transmission": 0.8 },
  { "name": "stone", "color": [480, 480, 500], "glossiness": 0.2, "physics": "stone" }
]
```

| Key | Type | Default | Packs into ([`Material`](../../concepts/materials.md#the-four-words)) |
|---|---|---|---|
| `name` | string | `""` | `Material::name` (not on the GPU) |
| `color` | `[r, g, b]`, each 0–1023 | `[0, 0, 0]` | word 0, R10G10B10 |
| `emission` | `[r, g, b]`, each 0–1023 | absent | word 1. Absent or all zero: emit in the `color`. |
| `glossiness` | 0–1 | 0 (fully rough) | word 2, byte 3 |
| `metallic` | 0–1 | 0 | word 2, byte 2 |
| `transparency` | 0–1 | 0 (opaque) | word 2, byte 1 |
| `ior` | 1.0–2.99 | 1.0 | word 2, byte 0, as `1 + raw/128` |
| `emissiveStrength` | ≥ 0, ~0.004–245 | 0 | word 3, byte 3, exponential |
| `transmission` | 0–1 | 0 | word 3, byte 2 |
| `flags` | integer 0–255 | 0 | word 3, byte 1: bit 0 animated, bits 1–4 motion set |
| `physics` | string | `""` | `Material::physics`: a name in the [physics material library](physics-materials-json.md) |

- **Colours are 10 bits per channel, sRGB-encoded**: what a colour picker, a texture or a `.vox`
  palette holds, stored as authored. An 8-bit `v` is `round(v × 1023 / 255)` (`packColor`). Renderers
  decode to linear in `decodeMaterial` (`include/pjv_color.sc`), shade linear, and encode once at
  display. Files written before 2026-10-22 hold `v × 4` (white 1020) and load unchanged. Plan:
  [color-convention.md](../../plans/color-convention.md).
- **The writer omits every property at its default**, so a colour-only palette writes only `name`
  and `color`.
- Values are quantised to their bytes on load; a load/save round trip is a fixed point.

## Mutability

| Value | Meaning today |
|---|---|
| `locked` (default) | Instances share a geometry blob with other `locked` instances of the same source |
| `direct` | Shares with other `direct` instances of the same source |
| `copy` | Shares with other `copy` instances of the same source |

The value is part of the [instancing key](#instancing), so instances with different policies
**never share a blob**: an edit to a `direct` instance can't reach a `locked` sibling. It is kept on
each chunk (`Chunk::mutability`) and written back on save.

!!! note "Not implemented: per-policy write-back"
    The intent is that `locked` is never written back, `direct` writes edits into the source `.data`,
    and `copy` writes them to a new file. No code does this yet: `saveComposeToDisk` writes every
    component's current voxels into the folder it is given, whatever the policy. See
    [plans/edit_p8.md](../../plans/edit_p8.md).

## Attachments

An `attachments` object, on the document or on any entry, holds data a **program** saves with the
scene and the engine never interprets.

```json
"attachments": {
  "projv.editor.csg": { "v": 1, "op": "subtract" },
  "mygame.bake":      { "v": 2, "quality": "high" }
}
```

- **Keys are namespaced.** `projv.` is reserved for programs in this repository.
- **Each value is an object carrying `"v"`**, its own schema version (absent means 1).
- **Unknown keys are preserved** through load and save.
- **Omitted when empty.**
- **Where it lands**: an entry's block belongs to that component; a folder's own block to the
  `asset` component that stands for it, or for the top-level folder, to the scene.

API: [`utils/attachments.h`](../../../include/utils/attachments.h). Attachments are for data about the
voxel structure written by tools. What a running program *does* goes in
[`entities.json`](entities-json.md).

### Legacy `op`

Older files had a top-level `"op"` on each entry (`none` / `union` / `subtract` / `intersect`). The
loader moves it into the `projv.editor.csg` attachment (`{"v": 1, "op": ...}`), verbatim, dropping
`none`. If both exist, the attachment wins. The next save writes the new form.

## Loading rules

### Path resolution

`source` resolves against the directory of the `compose.json` naming it, canonicalised; the canonical path keys caching and instancing.

### Instancing

| Cache | Key | Effect |
|---|---|---|
| `.data` files | canonical path | Each file is read once per load |
| Geometry blobs | canonical path + block grid coordinate + mutability | Every chunk with the same key shares **one** blob (refcounted) |

One entry is one instance. Ten entries naming `bird.data` give ten chunks and one blob.

### Owned vs linked assets

An `asset` whose folder is **inside** the document's folder is owned: saved as a sub-folder. One
**outside** it is a link (`ComponentRecord::externalSource`): expanded in memory, written back as a
bare reference to the folder, never copied.

### Recursion

Cycles (A includes B includes A) are allowed and warned about once each. Each level re-applies the
asset transform. Expansion stops at depth **6** (`MAX_RECURSION_DEPTH` in
[`src/utils/compose_io.cpp`](../../../src/utils/compose_io.cpp)).

### What a `data` entry becomes

| `.data` blocks | Component kind | In the Scene |
|---|---|---|
| 1 | `Chunk` | One loose chunk |
| more than 1 | `Grid` | One `SceneGrid`, one chunk per block (negative block coordinates are skipped) |
| 0 / unreadable | — | Warning, skipped |

## Worked example

```text
Example/
├── Scene/
│   ├── compose.json      terrain.data, ../Assets/Bird, ../Assets/Cow
│   └── terrain.data
└── Assets/
    ├── Bird/  compose.json, bird.data
    └── Cow/   compose.json, cow.data
```

`Scene/compose.json`:

```json
{
  "version": 1,
  "name": "Square Earth",
  "components": [
    { "type": "data",  "id": 1, "source": "terrain.data" },
    { "type": "asset", "id": 2, "source": "../Assets/Bird", "position": [110, 240, 30], "rotation": [45, 5, 0] },
    { "type": "asset", "id": 3, "source": "../Assets/Cow",  "position": [45, 80, 150],  "rotation": [90, 0, 0], "scale": 1.2 }
  ]
}
```

`Assets/Bird/compose.json`:

```json
{ "version": 1, "name": "Bird",
  "components": [ { "type": "data", "id": 1, "source": "bird.data",
                    "palette": [ { "name": "feather", "color": [900, 200, 120] } ] } ] }
```

Loading `Scene/` gives the terrain at the origin, a bird and a cow placed and rotated (both links,
since they live outside `Scene/`). Loading `Assets/Bird/` alone gives just the bird.

# File formats

Everything ProjectV reads or writes, byte for byte or key for key.

| File | Holds | Read / written by | Page |
|---|---|---|---|
| `compose.json` | Layout: components, transforms, palettes, attachments | `utils::loadComposeFromDisk` / `saveComposeToDisk` | [compose.json](compose-json.md) |
| `*.data` | Voxels: one tree64 (or a grid of them) + material bytes | `utils::readDataFile` / `writeDataFile` | [.data](data-file.md) |
| `entities.json` | Behaviour: entities and their ECS components | `runtime::spawnEntities` / `saveEntities` | [entities.json](entities-json.md) |
| `physics_materials.json` | The physics material library | `runtime::loadPhysicsMaterials` | [physics_materials.json](physics-materials-json.md) |
| `render.json`, `resources.json` | A renderer: passes, uniforms, shaders, textures, framebuffers | `graphics::loadRendererSpecification` | [Renderer folder](renderer-folder.md) |
| (inside `.data`) | The tree64 node array | the loader, the GPU | [tree64](tree64.md) |

## A compose folder on disk

A **folder is the unit of composition**. Point the loader at any folder with a `compose.json` and it
loads that folder and everything it references. A scene and an asset are the same thing.

```text
World/                         loadComposeFromDisk("World")
├── compose.json               components: terrain.data, water.data, ../Assets/Bird, House
├── entities.json              optional: entities linked to those components by "id"
├── terrain.data               a grid: many blocks → one SceneGrid
├── water.data
└── House/                     an asset this document owns (saved as a sub-folder)
    ├── compose.json
    ├── entities.json
    └── walls.data             one block → one loose chunk
Assets/
└── Bird/                      an asset referenced from outside: stays a link on save
    ├── compose.json
    └── bird.data
```

| Concern | Lives in |
|---|---|
| Where a thing is, what it is made of, what colours | `compose.json` |
| The voxels themselves | `.data` (no placement, no palette) |
| What it does at runtime | `entities.json` |
| Tool data about the voxel structure (editor booleans) | `compose.json` → `attachments` |

Which one to use for new data: [Where data goes](../../concepts/where-data-goes.md).

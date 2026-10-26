# Architecture

Six modules in one library, with strict rules about which may see which.

## Modules

```text
include/                      namespace            sees
├── core/              ───▶   projv, projv::core   EnTT, glm, spdlog            (no voxels, no GPU)
├── data_structures/   ───▶   projv                core, bgfx types              (plain data)
├── utils/             ───▶   projv::utils         core, data_structures         (the voxel layer)
├── net/               ───▶   projv::net           core                          (transport, wire format)
├── graphics/          ───▶   projv::graphics      everything below runtime      (window, GPU)
└── runtime/           ───▶   projv::runtime       everything                    (entities, physics, sessions)
```

| Module | What it is | Start with |
|---|---|---|
| **core** | The application, its loop and clock, events, maths, logging, paths. Knows nothing about voxels or the GPU. | [`core/application.h`](../../include/core/application.h) |
| **data_structures** | The plain types the rest operate on: `Scene`, `Chunk`, `GeometryBlob`, `GPUData`, renderer specifications. | [`data_structures/scene.h`](../../include/data_structures/scene.h) |
| **utils** | Everything voxel: compose I/O, the component tree, tile tables, editing, materials, picking, animation, collision geometry, loading part of a scene. | [`utils/compose_io.h`](../../include/utils/compose_io.h) |
| **net** | Moving bytes between machines: ENet and an in-process loopback behind one `Transport`, and the bit-level wire format. No EnTT, no runtime. | [`net/transport.h`](../../include/net/transport.h) |
| **graphics** | Window, input, bgfx, renderer descriptions, uploading the scene, drawing. | [`graphics/render_instance.h`](../../include/graphics/render_instance.h) |
| **runtime** | Entities linked to voxel components, authored entities, prefabs, network ids, world loading, replication (server and clients), physics. | [`runtime/scene_bridge.h`](../../include/runtime/scene_bridge.h) |

Per-header detail: [Reference](../reference/README.md).

## Layering rules

These are enforced by the `layering` ctest ([`tests/unit/check_layering.cmake`](../../tests/unit/check_layering.cmake)).
The compiler can't enforce them because EnTT is linked `PUBLIC`.

- **`utils/` and `data_structures/` never include EnTT, `core/world.h`, `core/application.h`,
  `core/events.h` or `runtime/`**, so tools without a game loop can load, edit and save scenes.
- **`runtime/` is the one layer that sees both** the voxel `Scene` and the entity `World`.
- **`net/` follows the same rule** (no EnTT, no runtime), and **only `src/net/transport_enet.cpp` includes ENet**.
- **Only `src/runtime/physics/` includes Jolt** (the `projv_physics` object library, built with
  Jolt's flags). No public header names a Jolt type.

```text
             ┌──────────────────────────── runtime ───────────────────────────┐
             │  bridge · entities.json · prefabs · NetId · loading · physics   │
             └───────┬─────────────────────────────────────────────┬──────────┘
                     │ sees                                        │ sees
   ┌─────────────────▼─────────────────┐            ┌──────────────▼──────────┐
   │ voxel layer: utils/ + data_struct │            │ core/world.h (EnTT)     │
   │ Scene, compose I/O, editing ...   │   ✗ never  │ core/application.h      │
   │                                   │ ─────────▶ │ core/events.h           │
   └───────────────────────────────────┘            └─────────────────────────┘
```

## Two worlds, linked

The engine keeps **what things are made of** apart from **what they are doing**:

| | The `Scene` | The `World` |
|---|---|---|
| Type | `projv::Scene` ([`data_structures/scene.h`](../../include/data_structures/scene.h)) | `projv::World` = `entt::registry` ([`core/world.h`](../../include/core/world.h)) |
| Holds | Voxel components in a hierarchy, their geometry and palettes | Entities and their ECS components |
| Saved as | `compose.json` + `.data` | `entities.json` |
| Used by | Every tool, with or without a game loop | Running programs |
| Joined by | The [Scene bridge](entities.md): an entity *links* one component; it never absorbs it | |

The `Scene` usually lives in `world.ctx()`.

## Conventions worth knowing early

- **Handles are rows, and freed rows are reused**: hold a `ComponentRef` across frames. See
  [The scene](scene.md#handles-refs-and-slot-reuse).
- **Create through the engine**: `utils::addComponent`, `reserveComponentSlot`/`placeComponent`,
  `allocateChunkSlot`, never `push_back`, so freed rows are reused.
- **What crosses machines is a `NetId`**, never an `Entity`, a handle or a body id. See
  [Entities](entities.md#netid-the-name-that-crosses-machines).
- **Physics input is checked, never asserted on**: everything reaches Jolt through `PhysicsWorld`,
  which refuses, clamps or logs first. See [Physics](physics.md).
- **Coding style**: [`CODING_STYLE.md`](../../CODING_STYLE.md).

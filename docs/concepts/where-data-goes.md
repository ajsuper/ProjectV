# Where data goes

Which file, field or component a new piece of data belongs in. The test: **who reads it, and what
is it about?**

## Decision table

| The data is... | It goes in | Saved in | Example |
|---|---|---|---|
| Read by the engine itself (it branches on it) | A `ComponentRecord` field | `compose.json` | `localPosition`, `localId`, `materialPalette` |
| About the **voxel structure**, written by a tool | An **attachment** ([`utils/attachments.h`](../../include/utils/attachments.h)) | `compose.json` (`"attachments"`) | The scene editor's boolean ops (`projv.editor.csg`), a primitive's parameters |
| What a running program **does** | An **ECS component** on an authored entity ([`runtime/entities.h`](../../include/runtime/entities.h)) | `entities.json` beside `compose.json` | Physics (`physics.rigidbody`), spawners, health, AI |
| What a voxel is **made of**, for physics | A physics material, named by palette entries | `physics_materials.json` (or code) | `stone`: density, friction, strength |
| How something **looks** per voxel | A palette entry (`Material`) | `compose.json` (`"palette"`) | Albedo, emission, glossiness |
| How the scene is **drawn** | The renderer folder | `render.json`, `resources.json`, shaders | Passes, render targets |
| The **voxels** | A geometry blob | `.data` | tree64 + material bytes |

## Rules

- **Never persist a `ComponentHandle`.** Files link by `localId` (the entry's `"id"`); networks by
  `NetId`. See [The scene](scene.md#handles-refs-and-slot-reuse).
- **Behaviour has one type**: the same component on disk and in the registry. See [Entities](entities.md).
- **Unknown data survives.** An attachment key or entity component nothing in this process knows
  is kept and written back.
- **Keys are namespaced.** `projv.` is reserved for this repository; use a prefix of your own
  (`mygame.spawn`).
- **Every value carries `"v"`**, its schema version (absent means 1).

## Attachments

Program data on a component or folder; the engine stores, copies and saves it, never interprets it.

```cpp
struct CsgRole { BooleanOp op = BooleanOp::None; };

template<> struct projv::utils::AttachmentTraits<CsgRole> {
    static constexpr const char*  key = "projv.editor.csg";
    static constexpr uint32_t     version = 1;
    static constexpr OnDuplicate  onDuplicate = OnDuplicate::Copy;     // or Drop
    static nlohmann::json         save(const CsgRole&);                // "v" is added for you
    static std::optional<CsgRole> load(const nlohmann::json&, uint32_t fileVersion);
};

utils::setAttachment(scene, handle, CsgRole{BooleanOp::Subtract});
if (const CsgRole* role = utils::getAttachment<CsgRole>(scene, handle)) { /* ... */ }
utils::removeAttachment<CsgRole>(scene, handle);
```

```json
{ "type": "data", "source": "cutter.data",
  "attachments": { "projv.editor.csg": { "v": 1, "op": "subtract" } } }
```

| Behaviour | Detail |
|---|---|
| Decoded lazily | Entries stay JSON text until their key is first asked for as a type |
| Refused values are kept | `load` returning `nullopt` logs once and keeps the raw text for save |
| Two scopes | `AttachmentScope::Component` (an entry) and `AttachmentScope::Document` (a folder's own block) |
| Duplicates | `duplicateComponent` honours each key's `OnDuplicate`; unknown keys are copied |
| Main thread only | A read may decode in place |

## Entity components

What runs is saved as itself:

```json
{ "name": "Spawner", "link": 3,
  "components": { "sandbox.spawner": { "v": 1, "interval": 1.1 } } }
```

How to define one: [Gameplay components](../guides/gameplay-components.md). File format:
[entities.json](../reference/formats/entities-json.md).

# Entities

The runtime: entities in an EnTT registry that **link** to voxel components, gameplay data saved as
the ECS components themselves, prefabs, and the one id that means the same thing on every machine.
Headers: [`runtime/scene_bridge.h`](../../include/runtime/scene_bridge.h),
[`runtime/entities.h`](../../include/runtime/entities.h),
[`runtime/net.h`](../../include/runtime/net.h).

## Entities link; they never absorb

```text
 World (entt::registry)                               Scene
 ┌──────────────────────────────────┐                ┌───────────────────────────────┐
 │ entity 12                        │   link (Root)  │ "House" (Asset)               │
 │   Transform  ────────────────────┼───────────────▶│   ├── "Walls" (Chunk)         │
 │   VoxelComponent{handle, Root}   │                │   └── "Door"  (Chunk) ◀──┐    │
 │   Health, Door, ...              │                │                          │    │
 │ entity 13                        │   link (Part)  │                          │    │
 │   Transform (in House's space) ──┼──────────────────────────────────────────┘    │
 │   VoxelComponent{handle, Part}   │                └───────────────────────────────┘
 └──────────────────────────────────┘
```

The Scene keeps its own hierarchy; an entity points at one component and moves its subtree.

| Link mode | Links to | `Transform` is |
|---|---|---|
| `LinkMode::Root` | A component with no Scene parent | Its world transform |
| `LinkMode::Part` | A component inside an asset (a door in a house) | In its Scene parent's space |

| `OnUnlink` | When the link goes (or the entity is destroyed) |
|---|---|
| `Keep` (default) | The component stays in the Scene |
| `Destroy` | The component is deleted too |

## The Scene bridge

```cpp
app.world.ctx().emplace<Scene>(utils::loadComposeFromDisk(folder));
runtime::installSceneBridge(app);                                   // once, before linking

Entity house = runtime::spawnComponent(app.world, houseHandle);     // create + link
app.world.patch<Transform>(house, [](Transform& t) { t.position.y += 1.0f; });
```

In PostUpdate the bridge writes every **changed** `Transform` into its component, unlinks deleted
components and updates `WorldTransform`. Flushing the GPU (`flushSceneUpdates`) is still yours.

- **Change `Transform` with `patch`, `replace` or `emplace_or_replace`.** Writing through
  `get<Transform>()` is invisible to the bridge.
- **One entity per component.** `linkComponent` refuses (and logs) a second link, a Root link to a
  parented component, or a dead one.
- **A deleted component unlinks its entity** and sends `ComponentDestroyed`; the entity stays.
- **`reseedTransforms(world)`** pulls component transforms back into entities after something else
  moved them.
- `entityFor(world, handle)` finds the linked entity.

## Authored entities: `entities.json`

`entities.json` sits beside `compose.json` and stores **the ECS components themselves**, with no
authored-only intermediate type.

```json
{ "version": 1,
  "entities": [
    { "name": "Sun",    "link": 1, "components": { "example.spin":  { "v": 1, "speed": 0.4 } } },
    { "name": "Rules",             "components": { "mygame.rules":  { "v": 1, "gravity": -28 } } },
    { "link": "document",          "components": { "sandbox.body":  { "v": 1, "radius": 1.5 } } }
  ] }
```

| `link` | Means |
|---|---|
| An integer | The `"id"` of an entry in **this folder's** `compose.json` (`ComponentRecord::localId`) |
| `"document"` | The node this folder becomes when loaded inside something else: a nested asset or a prefab |
| absent | Standalone entity |

Specialise `ComponentTraits` and register the type:

```cpp
struct Spin { float speed = 1.0f; };

template<> struct projv::runtime::ComponentTraits<Spin> {
    static constexpr const char* key = "example.spin";      // namespaced; "projv." is reserved
    static constexpr uint32_t version = 1;                  // written as "v"
    static nlohmann::json save(const Spin& s) { return {{"speed", s.speed}}; }
    static std::optional<Spin> load(const nlohmann::json& j, uint32_t fileVersion) {
        return Spin{j.value("speed", 1.0f)};                // nullopt refuses the value
    }
};

runtime::registerComponent<Spin>(app.world);                 // before spawning
runtime::spawnEntities(app.world);                           // every entities.json in the Scene
```

- **Unknown keys and refused values survive** (`UnknownComponents`) and are written back.
- **Inner folders spawn first**; an outer file naming the same component adds to and overrides that
  entity (prefab overrides).
- **Setup needing the entity** goes in `on_construct`, which runs after the link seeded `Transform`.
- **Saving**: `runtime::saveEntities(world, documentNode, folder)`. An entity belongs to the file it
  was spawned from (`Authored`), else to the file its linked component is an entry of. Linked
  entities' `Transform`s live in `compose.json`, not here.

Full format: [entities.json](../reference/formats/entities-json.md).
Walkthrough: [Gameplay components](../guides/gameplay-components.md).

## Prefabs

Any compose folder (plus optional `entities.json`), grafted into the running scene:

```cpp
Entity ball = runtime::instantiatePrefab(app.world, "prefabs/ball", {0, 10, 0});
app.world.destroy(ball);          // OnUnlink::Destroy by default: its voxels go too
```

- **Read once**, cached in `PrefabCache`. Disk changes need `runtime::clearPrefabCache(world)`.
- **Instances share one pinned blob**; editing one forks it.

Without the runtime, `utils::instantiateComposeInto(scene, folder, parent, ...)` does the graft
alone.

## `NetId`: the name that crosses machines

`Entity`, handles, refs and body ids are process-local. **Anything networked, replayed or compared
between peers uses `NetId`.**

```cpp
runtime::installNetIds(world);                 // installPhysics does this
NetId id = runtime::assignNetId(world, e);     // authority only: clients are told theirs
Entity e = runtime::entityForNetId(world, id);
```

Authored entities carry theirs as `"projv.netid": {"v": 1, "id": 7}`. Physics creates a tick's
bodies in `NetId` order, so registry order never matters.

## Events

| Event | When |
|---|---|
| `EntityLinked { entity, component }` | A link was made |
| `ComponentDestroyed { entity, component }` | The linked component was deleted; the link is already gone |
| `ComponentTransformChanged { entity, component }` | `reseedTransforms` changed an entity |

## Examples

[`examples/15-entities`](../../examples/15-entities/) (minimal), [`examples/16-sandbox`](../../examples/16-sandbox/)
(prefabs, physics, events).

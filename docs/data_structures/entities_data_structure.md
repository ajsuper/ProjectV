## ENTITIES - v1

`entities.json` holds a folder's **entities**: the gameplay side of an asset. It sits beside the
folder's `compose.json`, which holds the voxels. The voxel layer never reads it. The runtime does
(`runtime/entities.h`).

- [Why a second file](#why-a-second-file)
- [Format](#format)
- [Links](#links)
- [Components](#components)
- [Loading](#loading)
- [Saving](#saving)

---

### Why a second file

compose.json describes what an asset *is made of*. entities.json describes what it *does*. Keeping
them apart means:

- **The voxel layer stays free of the runtime.** The editor, the mesh voxelizer and every tool can
  load, edit and save a scene without an entity registry.
- **Each behaviour has one type.** An entity's components are the ECS components themselves, so the
  same data is what is on disk and what runs. There is no authored-only description to translate.
  The same serializers can carry a save game.

Data *about the voxel structure*, written by tools (the scene editor's boolean ops, say), is still
an [attachment](compose_data_structure.md#attachments) in compose.json. The test for which file a
piece of data belongs in: is it about the asset's structure (attachment) or about something the
running program does (entity component)?

### Format

```json
{
    "version": 1,
    "entities": [
        { "name": "Spawner", "link": 3, "onUnlink": "destroy",
          "components": { "sandbox.spawner": { "v": 1, "prefab": "ball", "interval": 1.1 } } },
        { "name": "Rules",
          "components": { "mygame.rules": { "v": 1, "gravity": -28 } } },
        { "name": "self", "link": "document",
          "components": { "sandbox.body": { "v": 1, "radius": 1.5, "mass": 1 } } }
    ]
}
```

| Field | Type | Required | Meaning |
|-------|------|----------|---------|
| `version` | integer | **yes** | Format version. Current: `1`. |
| `entities` | array | **yes** | The entities. May be empty. |
| `name` | string | no | Human identification, kept on load and save. |
| `link` | integer \| `"document"` | no | The voxel component this entity drives. See [Links](#links). Absent: the entity stands alone. |
| `onUnlink` | `"keep"` \| `"destroy"` | no (default `"keep"`) | `"destroy"`: deleting the entity deletes its component too. |
| `components` | object | no | The entity's components, by key. See [Components](#components). |

### Links

- **An integer** is the `"id"` of an entry in **this folder's** compose.json. Ids are unique within
  one file and survive reloads, renames and reorders. A component handle survives none of those,
  because it is just a position in memory for one load.
- **`"document"`** is the node this folder becomes when it is loaded as part of something else: a
  nested asset, or a prefab grafted at runtime. It is how a prefab's own data is attached to the
  prefab. In the folder that was opened at the top there is no such node, so a `"document"` entity
  there stands alone.

**Links are local to their folder.** The same folder can be loaded more than once (two houses, ten
copies of a prefab), and each copy resolves its file's links against its own node. An entity in an
outer folder cannot reach inside a nested one; the nested folder's own `entities.json` does that.

A link to a root component is a **Root** link (the entity's transform is the component's world
transform). A link to a component inside an asset is a **Part** link (its transform is in the
parent's space). See `runtime/scene_bridge.h`.

### Components

Each key names a component type a program registered (`runtime::registerComponent<T>`, with
`ComponentTraits<T>` saying how it reads and writes JSON). Each value is that component, as an
object, carrying `"v"`, its own schema version (absent means 1).

- **A key nothing registered is kept**, and written back on save. A tool that does not know a
  game's components cannot erase them.
- **A value its type refuses** (malformed, or from a version it cannot read) is kept the same way,
  with a warning.
- `projv.transform` (`position`, `rotation` as `[x, y, z, w]`, `scale`) is the engine's own. It
  places an entity with no link. On a linked entity it overrides the component's transform, and it
  is not written back, because a linked entity's transform lives in compose.json.

### Loading

`runtime::spawnEntities` reads every folder of the scene: the top-level folder, and every node that
stands for a folder. **Inner folders go first.** When an outer folder's file names a component
that an inner one already gave an entity, the outer entity's components are **added to that same
entity**, overriding any it shares, rather than creating a second entity to fight it for the
transform. That is the prefab-override rule.

`runtime::instantiatePrefab` grafts a folder into the running scene and spawns its file in one
call.

### Saving

`runtime::saveEntities(world, node, folder)` writes one folder's file. **Every entity belongs to
exactly one file:**
- an entity spawned from a file belongs to that file;
- an entity made in code belongs to the file its linked component is an *entry* of, its parent's.

An entity linked to a nested folder's node is therefore written in the outer file (by id), never
again inside the nested one. To make the nested folder own it, as a `"document"` entity, give it an
`Authored` naming that folder's node.

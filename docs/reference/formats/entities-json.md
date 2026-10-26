# entities.json

A folder's entities: the gameplay side of an asset, beside the `compose.json` that holds its voxels.
The voxel layer never reads it; the runtime does ([`runtime/entities.h`](../../../include/runtime/entities.h)).
Concepts: [Entities](../../concepts/entities.md).

```json
{
  "version": 1,
  "entities": [
    { "name": "Spawner", "link": 3, "onUnlink": "destroy",
      "components": { "sandbox.spawner": { "v": 1, "prefab": "ball", "interval": 1.1 } } },
    { "name": "Rules",
      "components": { "mygame.rules": { "v": 1, "gravity": -28 },
                      "projv.transform": { "position": [0, 5, 0], "rotation": [0, 0, 0, 1], "scale": 1 } } },
    { "name": "self", "link": "document",
      "components": { "physics.rigidbody": { "v": 1, "motion": "dynamic", "density": 600 } } }
  ]
}
```

Comments (`//`, `/* */`) are accepted when reading.

## Keys

| Key | Type | Default | Meaning |
|---|---|---|---|
| `version` | integer | **required** | Must be `1`, or the file is skipped with an error |
| `entities` | array | `[]` | The entities, spawned in file order |
| `name` | string | `""` | For people; kept on load and save (`Authored::name`) |
| `link` | integer \| `"document"` | absent | The voxel component this entity drives. See [Links](#links). |
| `onUnlink` | `"keep"` \| `"destroy"` | `"keep"` | `"destroy"`: deleting the entity deletes its component too |
| `components` | object | `{}` | The entity's components, by key. See [Components](#components). |

## Links

| `link` | Resolves to | Link mode |
|---|---|---|
| integer | The entry of **this folder's** `compose.json` with that `"id"` | Root if it is top-level, Part if it is inside an asset |
| `"document"` | The node this folder becomes when loaded inside something else (a nested asset, a prefab) | as above |
| absent | Nothing: a standalone entity | |

- **Links are local.** Each copy of a folder loaded twice resolves its own file against its own node.
  An outer file can't reach inside a nested folder; the nested folder's own file does that.
- **An id the folder doesn't have** spawns the entity unlinked, with a warning. Its link is kept as
  written and saved back.
- **`"document"` in the top-level folder** has no node, so the entity stands alone.

## Components

Each key is a component type a program registered with `runtime::registerComponent<T>` and a
`ComponentTraits<T>` specialisation. Each value is an object with `"v"`, its schema version (absent
means 1).

- **A key nothing registered is kept** (`UnknownComponents`) and written back on save.
- **A value its type refuses** (`load` returns `nullopt` or throws a JSON error) is kept the same way,
  with a warning.

Keys the engine registers:

| Key | Type | Registered by | Fields |
|---|---|---|---|
| `projv.transform` | `Transform` | always | `position [x,y,z]`, `rotation [x,y,z,w]`, `scale`. Places an unlinked entity; overrides a linked one's component transform on spawn; **not saved for linked entities** (that lives in `compose.json`). |
| `projv.netid` | `NetId` | `installNetIds` / `installPhysics` | `id` (non-zero) |
| `physics.rigidbody` | `RigidBody` | `installPhysics` | `motion`, `shape`, `radius`, `halfExtents`, `halfHeight`, `density`, `mass`, `friction`, `restitution`, `linearDamping`, `angularDamping`, `gravityFactor`, `layer`, `quality`, `material`, `breaksOnImpact`, `quality`, `material`, `breaksOnImpact`, `minImpactSpeed`, `minImpactEnergy`, `senses`, `blockedByStatic` |
| `physics.static` | `StaticCollider` | `installPhysics` | `friction`, `restitution`, `layer`, `splits`, `anchorBase`, `material`, `breaksOnImpact`, `minImpactSpeed`, `minImpactEnergy`, `senses` |
| `physics.character` | `CharacterController` | `installPhysics` | `radius`, `height`, `maxSlopeDegrees`, `mass`, `strength`, `stepHeight`, `layer` |
| `physics.joint` | `Joint` | `installPhysics` | Joint type, bodies by NetId, anchors, limits, motor, break thresholds |
| `physics.groups` | `CollisionGroups` | `installPhysics` | `groups`, `ignores` (group names, `definePhysicsGroup`), `family` |
| `physics.contacts` | `ContactReports` | `installPhysics` | `only`: group names; the body reports contacts with those |
| `physics.frozen` | `FrozenMotion` | `installPhysics` | `linear`, `angular`: the motion world loading froze, given back when the body is active |

Field meanings and defaults are on the structs in [`runtime/physics.h`](../../../include/runtime/physics.h).
Some fields are omitted on save when they hold their default.

## Loading

`runtime::spawnEntities(world)` reads the top-level folder's file and every file of every node that
stands for a folder. **Inner folders go first.** When an outer file names a component an inner file
already gave an entity, the outer components are **added to that entity**, overriding any it
shares, instead of making a second entity. That is the prefab-override rule.

`runtime::instantiatePrefab` grafts a folder and spawns its file in one call.

## Saving

`runtime::saveEntities(world, document, folder)` writes one folder's file. Every entity belongs
to exactly one file:

| Entity | Belongs to |
|---|---|
| Spawned from a file (has an `Authored`) | That file |
| Made in code, linked | The file its linked component is an **entry** of (its parent's folder) |
| Made in code, unlinked | Nobody, until given an `Authored` |

An entity linked to a nested folder's node is written in the outer file by id. To make the nested
folder own it as a `"document"` entity, give it an `Authored` naming that node. Entities are written
in their original file order, then the rest in entity order.

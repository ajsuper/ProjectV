# physics_materials.json

The project's physics material library: what voxels are made of, as physics sees it. Loaded by
`runtime::loadPhysicsMaterials(world, path)` ([`runtime/physics.h`](../../../include/runtime/physics.h)).
Each entry becomes a `PhysicsMaterial`
([`runtime/physics/physics_world.h`](../../../include/runtime/physics/physics_world.h)). How bodies use
it: [Physics](../../concepts/physics.md).

```json
{
  "v": 1,
  "materials": [
    { "name": "ice",    "density": 917,  "friction": 0.01 },
    { "name": "rubber", "density": 1100, "friction": 1.0, "restitution": 0.9 },
    { "name": "brick",  "density": 1900, "friction": 0.7,
      "strength": 40000, "fracture": "seams", "shardSize": 0.35 },
    { "name": "timber", "density": 600,
      "strength": 40000, "fracture": "splinter", "shardSize": 0.4, "grain": [0, 1, 0] }
  ]
}
```

## Keys

| Key | Type | Default | Meaning |
|---|---|---|---|
| `v` | integer | 1 | Library version. Only 1 is read. |
| `materials` | array | **required** | Defined in file order |
| `name` | string, 1–64 chars | **required** | What palette entries (`"physics"` in [compose.json](compose-json.md#palette)) and `RigidBody::material` name |
| `density` | kg/m³, > 0 | 1000 | Mass of the voxels made of it |
| `friction` | ≥ 0 | 0.6 | Combined per contact as the geometric mean |
| `restitution` | 0–1 (clamped) | 0 | Bounce; combined as the larger of the two |
| `strength` | J/m³, ≥ 0 | 0 | Energy to break a cubic metre. **0: never breaks.** |
| `fracture` | string | `"shatter"` | Fracture pattern: `shatter`, `splinter`, `crumble`, `seams`, or one registered with `registerFracturePattern` |
| `shardSize` | m, > 0 | 0.5 | About how big a shard is |
| `grain` | `[x, y, z]`, non-zero | `[0, 1, 0]` | The direction `splinter` runs along, in the component's local axes (normalised) |
| `anchor` | boolean | false | Holds up what it is part of: in a `splits` static collider, a piece with a voxel of an anchor material stands whatever is cut away under it (bedrock, a foundation) |

## Rules

- **Indices follow file order** (1, 2, 3, ...; 0 is "none"), so peers loading the same file agree.
  The library is part of the physics state hash.
- **Loading stops at the first bad entry**: no name, or a value of the wrong type. Entries before it
  stay defined; the call returns false and logs why.
- **Defining a name again changes it in place**, and every body using it rebuilds at the next step.
- **At most 1023 materials** (`MAX_PHYSICS_MATERIALS` − 1).
- A material with a `strength` whose `fracture` names no registered pattern is accepted, with a
  warning.

In code, without a file:

```cpp
runtime::definePhysicsMaterial(world, "ice", {.density = 917.0f, .friction = 0.01f});
```

The bundled library: [`examples/17-physics/physics_materials.json`](../../../examples/17-physics/physics_materials.json).

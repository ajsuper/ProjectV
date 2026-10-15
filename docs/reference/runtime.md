# runtime

The one layer that sees both the voxel `Scene` and the entity `World`: the Scene bridge, authored
entities and prefabs, network ids, physics and fracture. Functions are in `projv::runtime`; the
ECS component types (`Transform`, `RigidBody`, `NetId`, ...) are in `projv`.

**Only `src/runtime/physics/` includes Jolt**, and no public header names a Jolt type (`layering`
ctest).

Explained in: [Entities](../concepts/entities.md), [Physics](../concepts/physics.md).
Formats: [entities.json](formats/entities-json.md), [physics_materials.json](formats/physics-materials-json.md).

## `runtime/scene_bridge.h`

[`include/runtime/scene_bridge.h`](../../include/runtime/scene_bridge.h): entities linked to voxel
components.

| Name | What |
|---|---|
| `Transform`, `WorldTransform` | Local transform (change it with `patch`); derived matrix |
| `VoxelComponent` | The link: `handle`, `mode`, `onUnlink`, `generation` |
| `LinkMode` | `Root` (world transform), `Part` (in the Scene parent's space) |
| `OnUnlink` | `Keep`, `Destroy` |
| `installSceneBridge(app)` | Signals + the PostUpdate `"scene bridge"` system. Once, before linking. |
| `linkComponent`, `unlinkComponent`, `spawnComponent` | Make and remove links |
| `entityFor(world, handle)` | The entity linked to a component |
| `reseedTransforms(world)` | Read components back into entities |
| `EntityLinked`, `ComponentDestroyed`, `ComponentTransformChanged` | Events |

## `runtime/entities.h`

[`include/runtime/entities.h`](../../include/runtime/entities.h): `entities.json` and prefabs.

| Name | What |
|---|---|
| `ComponentTraits<T>` | Specialise: `key`, `version`, `save`, `load` |
| `registerComponent<T>(world)` | Make `T` loadable and savable |
| `spawnEntities(world)`, `spawnEntitiesUnder(world, node)` | Spawn every `entities.json` (inner folders first) |
| `saveEntities(world, document, folder)` | Write one folder's file |
| `instantiatePrefab(world, folder, pos, rot, scale, onUnlink)` | Graft a folder and spawn its entities; cached and shared |
| `PrefabCache`, `clearPrefabCache(world)` | The cache; forget it |
| `Authored` | Where an entity came from (which file, link, order) |
| `UnknownComponents` | Keys nothing registered, kept and written back |
| `SpawnedEntities` | What a spawn made, in order |

## `runtime/net.h`

[`include/runtime/net.h`](../../include/runtime/net.h): the id that crosses machines.

| Name | What |
|---|---|
| `NetId` | `value`, 0 = none. Saved as `"projv.netid"`. |
| `installNetIds(world)` | Register + start the allocator (`installPhysics` calls it) |
| `assignNetId(world, entity)` | Authority only |
| `entityForNetId(world, id)` | |

## `runtime/physics.h`

[`include/runtime/physics.h`](../../include/runtime/physics.h): physics for entities.

| Name | What |
|---|---|
| `installPhysics(app, config)` | `"physics"` in FixedUpdate, `"physics: present"` in PostUpdate before the bridge |
| `PhysicsConfig` | `settings`, `collision`, `destroyBodiesThatLeaveTheWorld`, `maxDebris` |
| `RigidBody` (`"physics.rigidbody"`) | Dynamic or kinematic; shape `Voxels`/`Sphere`/`Box`/`Capsule`, density/mass, friction, `material`, `breaksOnImpact` |
| `StaticCollider` (`"physics.static"`) | Immovable voxels; `splits` makes unsupported pieces fall |
| `CharacterController` (`"physics.character"`) | A walking capsule |
| `Joint` (`"physics.joint"`) | Fixed / point / hinge / slider / distance between two `NetId`s |
| `ConstantVelocity` | A kinematic body that flies on by itself |
| `PhysicsBody`, `PhysicsCharacter`, `PhysicsJoint`, `PhysicsVoxelPart`, `Debris` | The simulation's side; never saved |
| `addImpulse`, `addAngularImpulse`, `addVelocity`, `setVelocity`, `teleport`, `setGravity` | Commands, applied at the next fixed step in issue order |
| `setCharacterInput`, `characterState` | Characters |
| `connectBodies`, `setJointMotor`, `jointState` | Joints |
| `editVoxels`, `voxelsInSphere`, `voxelComponentOf` | Voxel edits on a tick |
| `fracture(world, entity, at, energy, push)` | Break a voxel body |
| `definePhysicsMaterial`, `loadPhysicsMaterials`, `physicsMaterial`, `physicsMaterialIndex` | The material library |
| `submitCommand(PhysicsCommand)`, `nextPhysicsTick`, `lateCommands` | Commands from elsewhere, by `NetId` and tick |
| `physicsStateHash`, `savePhysics`, `restorePhysics` | Compare and rewind |
| `castRay`, `castSphere`, `overlapSphere`, `overlapBox` → `QueryHit` | Queries, traced to the voxel |
| `bodyPose`, `hasBody`, `physicsWorld(world)` | State; the raw `PhysicsWorld` |
| `PhysicsBodyRefused`, `BodyLeftWorld`, `BodySplit`, `BodyFractured`, `BodyEmptied`, `JointBroke`, `SimulationSlow` | Events |

## `runtime/fracture.h`

[`include/runtime/fracture.h`](../../include/runtime/fracture.h): how a material breaks.

| Name | What |
|---|---|
| `FracturePattern` | `(FractureInput, FractureRandom&) → shard index per voxel` (−1 stays). Must be deterministic. |
| `registerFracturePattern(world, name, fn)`, `fracturePattern(world, name)` | Register; look up |
| `fracture_patterns::shatter`, `splinter`, `crumble`, `seams` | The four built in |
| `FractureInput`, `FractureVoxel`, `FractureRandom` | What a pattern is given; its only randomness |
| `fractureShards(input, shardOf, maxExtent, maxShards)` | What the engine makes of a pattern's answer |

## `runtime/physics/physics_world.h`

[`include/runtime/physics/physics_world.h`](../../include/runtime/physics/physics_world.h): the
Jolt simulation behind an interface that names no Jolt type. Deterministic, restorable, refuses bad
input. Path: `runtime/physics/physics_world.h`.

| Name | What |
|---|---|
| `PhysicsWorld` | `createBody`/`destroyBody`, `step(dt)`, `tick`, `bodyState`, impulses and velocities, `setPose`, `moveKinematic` |
| | `saveState`/`restoreState`, `stateHash`, `recording`, `replay(log)` |
| | `voxelShape(blob, res, params, materials)` (cached by content stamp), `setMaterial` |
| | characters, joints, `castRay`/`castSphere`/`overlap*`, `impacts()`, `stats()`, `takeBodiesThatLeftTheWorld` |
| `PhysicsSettings` | Capacities, gravity, world bounds, speed limits, rest damping, solver steps, `minImpactSpeed`, `record`, `threads` |
| `BodyDesc`, `BodyId`, `BodyState`, `MotionType`, `PhysicsLayer` | Bodies |
| `PhysicsShape`, `PhysicsShapePart`, `CollisionShape` | Box, sphere, capsule, voxels, compound |
| `PhysicsMaterial`, `MAX_PHYSICS_MATERIALS` | density, friction, restitution, strength, fracture, shardSize, grain |
| `CharacterDesc`/`Id`/`State`, `JointDesc`/`Id`/`State`, `JointType`, `MotorMode` | Characters and joints |
| `QueryFilter`, `PhysicsHit`, `Impact`, `PhysicsStats`, `PhysicsLog`, `ReplayResult` | Queries, contacts, diagnostics, replays |

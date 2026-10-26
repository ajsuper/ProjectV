# runtime

The one layer that sees both the voxel `Scene` and the entity `World`: the Scene bridge, authored
entities and prefabs, network ids, world loading, replication, physics and fracture. Functions are in `projv::runtime`; the
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

## `runtime/loading.h`

[`include/runtime/loading.h`](../../include/runtime/loading.h): world loading for a running program,
`utils::SceneLoader`'s blocks plus the entities that go with them. Plan:
[world-loading](../plans/world-loading.md).

| Name | What |
|---|---|
| `openWorld(app, folder, config)` | Open a compose folder lazily (no voxels), install the bridge, add `"world loading"` to PreUpdate. Then `spawnEntities` as usual. |
| `WorldLoadingConfig` | `loader` (`utils::LoaderConfig`), `policy` |
| `setLoadRule`, `setViewers`, `worldLoader`, `hasWorldLoading` | The rule, the points the world is seen from, the loader |
| `updateWorldLoading`, `settleWorld` | One pass now; load everything the rule wants before returning |
| `LoadViewer`, `LoadBounds`, `LoadState` | Components: a viewer entity; a box for an unlinked entity; the rule's answer (`Active`/`Kept`) |
| `EntityUnloading`, `EntityReloaded` | Events either side of an entity leaving and returning (new `Entity`, same `NetId`) |

## `runtime/replication.h`

[`include/runtime/replication.h`](../../include/runtime/replication.h): a session over a
[`net::Transport`](net.md): entities the server makes reach every client by `NetId`, and the clients'
input reaches the server. Plan: [networking](../plans/networking.md).

| Name | What |
|---|---|
| `installServer(app, transport, ServerConfig)`, `installClient(app, transport, ClientConfig)` | Start a side. Policy is the game's: `spawnPlayer`, `applyInput`, `onMessage` |
| `Replicated` | On a server entity: sent to every client; `omit` names components not sent |
| `NetProxy` | A client's stand-in for anything it does not own, shown between snapshots |
| `setLocalInput(world, move, jump)` | A client's input, each frame |
| `sendToServer`, `sendToClient`, `sendToClients` | The game's own messages |
| `setClientLoadRule` | What a client is sent: only what is near its player |
| `sceneFilesHash(folder)` | The handshake's check that both sides have the same files |
| `clientStats`, `SessionStats`, `ClientStats` | Bytes, mismatches, prediction corrections |
| `ClientJoined`, `ClientLeft`, `JoinedServer`, `LeftServer` | Events |

Voxel edits and breaks of entities with a `NetId` are replicated as what happened (`VoxelsEdited`,
`BodyBroken`), in order, and a client that joins later gets the lot. A client simulates only what it
owns.

## `runtime/physics.h`

[`include/runtime/physics.h`](../../include/runtime/physics.h): physics for entities. Every behaviour is
asked for, not assumed.

| Name | What |
|---|---|
| `installPhysics(app, config)` | `"physics"` in FixedUpdate, `"physics: present"` in PostUpdate before the bridge. Idempotent. |
| `PhysicsConfig` | `settings`, `collision`, `destroyBodiesThatLeaveTheWorld`, `maxPieces`, `dustVoxels`, `debris` (`DebrisConfig`), `fracture` (`FractureLimits`) |
| `RigidBody` (`"physics.rigidbody"`) | Dynamic or kinematic; shape `Voxels`/`Sphere`/`Box`/`Capsule`, density/mass, friction, restitution, `material`, `quality`, `breaksOnImpact`, `minImpactSpeed`, `minImpactEnergy`, `senses` (sensor layer), `blockedByStatic` |
| `StaticCollider` (`"physics.static"`) | Immovable voxels; `splits` makes unsupported pieces fall, `anchorBase`, and the same impact and sensor fields |
| `CharacterController` (`"physics.character"`) | A walking capsule |
| `Joint` (`"physics.joint"`) | Fixed / point / hinge / slider / distance between two `NetId`s |
| `ConstantVelocity` | A kinematic body that flies on by itself |
| `CollisionGroups`, `ContactReports` | Groups, ignores and a family; which contacts a body reports (`only`: group names) |
| `FrozenMotion` | The motion a body had when world loading froze it, given back when it is active |
| `PhysicsBody`, `PhysicsCharacter`, `PhysicsJoint`, `PhysicsVoxelPart`, `PhysicsHold`, `Debris` | The simulation's side; never saved |
| `definePhysicsGroup`, `physicsGroup` | Collision group names ↔ bits |
| `addImpulse`, `addAngularImpulse`, `addVelocity`, `setVelocity`, `teleport`, `setGravity` | Commands, applied at the next fixed step in issue order |
| `holdBody`, `isHeld`, `driveBody`, `setBodyState` | Held still; driven to a pose each step (what a client does to a networked proxy); set outright |
| `setCharacterInput`, `characterState` | Characters |
| `connectBodies`, `setJointMotor`, `jointState` | Joints |
| `editVoxels`, `applyVoxelEdit`, `acceptVoxelChanges`, `voxelsInSphere`, `voxelComponentOf` | Voxel edits on a tick |
| `fracture(world, entity, strike)`, `fracture(world, entity, at, energy, push)`, `fractureWithin`, `canBreak` | Break a voxel body. `Strike`: `at`, `energy` or `reach`, `push`, `minimumBreak`, `shardSize`, `cause`, `striker`, `returnUnusedEnergy` |
| `applyVoxelSplit` | Make a break's pieces from what a server sent (`VoxelSplit`), without a support pass |
| `impacts(world)` → `BodyImpact` | The last step's collisions: point, normal, closing speed, energy, load share, impulse |
| `contacts(world)` → `BodyContact`, `touching(world, entity)` | Contacts began/ended; what is on a plate |
| `installImpactFracture(app, config)` | Adds `"physics: impact fracture"`: each impact on a `breaksOnImpact` body strikes it with the collision's energy (`ImpactFractureConfig`) |
| `definePhysicsMaterial`, `loadPhysicsMaterials`, `physicsMaterial`, `physicsMaterialIndex` | The material library |
| `submitCommand(PhysicsCommand)`, `nextPhysicsTick`, `lateCommands` | Commands from elsewhere, by `NetId` and tick |
| `physicsStateHash`, `savePhysics`, `restorePhysics` | Compare and rewind (**not** across anything that makes or removes bodies) |
| `castRay`, `castSphere`, `overlapSphere`, `overlapBox` → `QueryHit` | Queries, traced to the voxel |
| `bodyPose`, `hasBody`, `physicsWorld(world)`, `hasPhysics` | State; the raw `PhysicsWorld` |

Events (`core::Events`): `PhysicsBodyRefused`, `BodyLeftWorld`, `BodyBroken` (`cause`: `Edit`,
`Support`, `Strike`, `Impact`, `Replicated`; delivered **inside the step**, before the pieces' bodies
exist, so a handler can shape them), `PiecesCleared`, `ContactBegan`, `ContactEnded`, `BodyEmptied`,
`VoxelsEdited`, `JointBroke`, `SimulationSlow`. A break's pieces carry `Piece`.

Concepts: [Physics](../concepts/physics.md). Plans: [cheap-debris](../plans/cheap-debris.md),
[impact-loads](../plans/impact-loads.md), [voxel-collision-shape](../plans/voxel-collision-shape.md).

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

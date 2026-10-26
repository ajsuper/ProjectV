# Physics

Rigid bodies made from the voxels an entity links, simulated by Jolt in FixedUpdate. Headers:
[`runtime/physics.h`](../../include/runtime/physics.h) (the ECS side),
[`runtime/physics/physics_world.h`](../../include/runtime/physics/physics_world.h) (the core),
[`runtime/fracture.h`](../../include/runtime/fracture.h),
[`utils/collision_geometry.h`](../../include/utils/collision_geometry.h). Design and history:
[plans/physics-jolt.md](../plans/physics-jolt.md).

## The picture

```text
 entities.json / code                FixedUpdate "physics"                  PostUpdate
 ┌──────────────────────┐   ┌──────────────────────────────────────┐   ┌────────────────────────┐
 │ RigidBody            │   │ 1. make bodies for new components    │   │ "physics: present"     │
 │ StaticCollider       │──▶│    (NetId order)                     │──▶│  Transform = lerp(prev,│
 │ CharacterController  │   │ 2. apply commands (tick, issuer, seq)│   │   cur, fixedAlpha)     │
 │ Joint                │   │ 3. PhysicsWorld::step(fixedDelta)    │   │ then "scene bridge"    │
 │ ConstantVelocity     │   │ 4. impacts → fracture, splits, events│   │  moves the voxels      │
 └──────────────────────┘   └──────────────────────────────────────┘   └────────────────────────┘
          ▲ voxel shape: utils::buildCollisionPieces(blob) → boxes, cached by contentStamp
```

```cpp
runtime::installSceneBridge(app);
runtime::installPhysics(app);            // physics world in world.ctx(), the two systems, the components
// entities.json: "physics.rigidbody": { "v": 1, "motion": "dynamic", "density": 600 }
//                "physics.static":    { "v": 1 }
runtime::addImpulse(app.world, crate, {0, 40, 0});
```

## What makes something solid

**Nothing collides that was not asked to.** Installing physics changes nothing in a scene until an
entity carries one of these:

| Component | Saved as | Is |
|---|---|---|
| `RigidBody` | `physics.rigidbody` | Something that moves: `Dynamic` (by the simulation) or `Kinematic` (by its `Transform`, pushing what it meets) |
| `StaticCollider` | `physics.static` | Immovable geometry: floors, walls, terrain |
| `CharacterController` | `physics.character` | A walking capsule, moved by input rather than forces |
| `Joint` | `physics.joint` | A constraint between two bodies named by `NetId`, on an entity of its own |
| `ConstantVelocity` | (code only) | A kinematic body that flies on by itself at `velocity` / `spin` |

Physics adds `PhysicsBody` / `PhysicsCharacter` / `PhysicsJoint` alongside: internal, never saved.

## Shapes come from the voxels

- **The default shape is the linked component's voxels**: every chunk under it, grid cells
  included, merged greedily into boxes (`utils::buildCollisionPieces`).
- **Shapes are shared**: every body made from the same blob content uses one, cached by
  `GeometryBlob::contentStamp`. An edit gives the blob a new stamp, and the shape is rebuilt at the
  next step.
- **Primitives on request**: `RigidBody::shape` = `Sphere`, `Box` or `Capsule`, fitted to the
  voxels' content box unless sized (`radius`, `halfExtents`, `halfHeight`).
- **Over budget** (`CollisionParams::pieceBudget`, default 1024 pieces): the blob collides as its
  content box, flagged and logged.

Collision-piece rules every representation keeps: never bigger than the voxels, deterministic, and
normals come from the pieces, never from voxel coordinates.

## One writer per pose

| Body | Moved by | Patching its `Transform` |
|---|---|---|
| Dynamic | The simulation, which writes its `Transform` | Does nothing (logged once). Use `teleport`. |
| Kinematic | Its `Transform` (or `ConstantVelocity`) | Moves it, pushing what it meets |
| Static collider | Its component | Stays where the component is |
| Character | `setCharacterInput` | Physics writes the position; the rotation (facing) is the game's |

## Everything happens at tick boundaries

```text
frame:   Update ── addImpulse(e) ──┐
                                   ▼  stamped (next tick, issuer 0, sequence n)
tick T:  [new bodies in NetId order] → [commands in (tick, issuer, sequence) order] → step
```

- **Bodies** for components that appeared are made at the start of the next fixed step, in NetId
  order (then entity order). **Destroyed entities' bodies** go then too.
- **Commands** apply at the start of the next fixed step, in the order issued. An entity with no
  body by then is skipped.
- So **the simulation is a function of the commands and their ticks**: the basis for replays and
  networking.

| Command | Does |
|---|---|
| `addImpulse`, `addAngularImpulse` | N·s at the centre of mass; N·m·s |
| `addVelocity`, `setVelocity` | A velocity change whatever the mass; set outright |
| `teleport(world, e, position, rotation)` | Move without sweeping; velocity kept; `Transform` follows uninterpolated |
| `setGravity(world, g)` | World gravity |
| `setCharacterInput(world, e, velocity, jump, jumpSpeed)` | Walk horizontally; jump once when next standing |
| `connectBodies(world, a, b, joint)` | A joint entity; gives `a` and `b` NetIds if they lack them |
| `setJointMotor(world, joint, on, value, mode)` | Drive a hinge or slider |
| `editVoxels(world, e, ops)` | Voxel edits on the step (see [below](#editing-bodies)) |
| `fracture(world, e, at, energy, push)` | Break voxels at a point (see [Fracture](#fracture)) |

From a server, a peer or a replay, the same commands go through
`submitCommand(world, PhysicsCommand{...})`, named by `NetId` and stamped with their tick. One for a
tick already run applies at the next and is counted in `lateCommands(world)`.

## What is drawn is interpolated

The simulation runs at `Time::fixedDelta`. In PostUpdate, before the Scene bridge, each moving
body's `Transform` is set between its last two fixed poses by `Time::fixedAlpha`. **Gameplay that
needs where a body is reads
`bodyPose(world, e)`**, the end of the last step, not the drawn pose.

## Queries

Against the simulation as of the last fixed step:

```cpp
if (auto hit = runtime::castRay(world, origin, direction, 80.0f, 0xffffffffu, camera.walker)) {
    hit->entity;  hit->position;  hit->normal;  hit->netId;
    if (hit->onVoxel) { hit->component; hit->chunk; hit->voxel; hit->materialSlot; }
}
runtime::castSphere(world, origin, radius, direction, maxDistance);
runtime::overlapSphere(world, centre, radius);              // NetId order, then entity order
runtime::overlapBox(world, centre, halfExtents, rotation);
runtime::bodyPose(world, e);   runtime::hasBody(world, e);
runtime::characterState(world, e);   runtime::jointState(world, joint);
```

`layers` is a bit per `PhysicsLayer` (`QueryFilter::only(PhysicsLayer::Moving)`); `ignore` skips
one entity's body (the caster's own).

| Layer | Collides with |
|---|---|
| `Static` | Everything except `Static` |
| `Moving` | Everything |
| `Sensor` | Nothing: it reports what enters and leaves (see [Contacts and sensors](#contacts-and-sensors)) |
| `Character` | Everything |

Finer than layers: [collision groups](#collision-groups).

## Materials

A project-wide **library** of named physics materials. Palette entries name one
(`Material::physics`), and so can bodies (`RigidBody::material`, `StaticCollider::material`).

```cpp
runtime::loadPhysicsMaterials(world, "physics_materials.json");
runtime::definePhysicsMaterial(world, "ice", {.density = 917, .friction = 0.01f});
```

| `PhysicsMaterial` field | Default | Meaning |
|---|---|---|
| `density` | 1000 | kg/m³ |
| `friction` | 0.6 | Combined as the geometric mean |
| `restitution` | 0 | 0..1; combined as the larger |
| `strength` | 0 | J to break a cubic metre; 0 never breaks |
| `fracture` | `"shatter"` | Pattern name |
| `shardSize` | 0.5 | m, about how big a shard is |
| `grain` | (0, 1, 0) | Direction splinters run, in the component's frame |

- **A body whose voxels mix materials** weighs what each part is made of, and each contact uses
  the material of the voxels touched. A palette entry's material wins for its voxels; the body's
  is for the rest (and for primitives).
- **Indices follow definition order**; the library is in the state hash, so mismatched peers are
  detected.
- **Redefining a name** changes it in place; every body using it is rebuilt at the next step.

File format: [physics_materials.json](../reference/formats/physics-materials-json.md).

## Fracture

```cpp
runtime::fracture(world, wall, hitPoint, 4000.0f /* J */, direction * 3.0f /* push, m/s */);
```

```text
strike at `at` with energy E
  └─ struck voxel (nearest within 3 voxels) → its material's strength
       └─ reach = radius of the sphere holding E / strength m³   (capped at 64 voxels)
            └─ pattern(voxels within 1.5 × reach) → shard number per voxel, or -1 (stays)
                 └─ engine: unbreakable voxels stay, shards cut to face-connected parts,
                            split if > 64 voxels across, each shard a new dynamic body
```

| Built-in pattern | For | Breaks into |
|---|---|---|
| `shatter` | stone, glass | Voronoi cells, small near the strike, `shardSize` at the reach; jagged edge |
| `splinter` | wood | As shatter, cells 3× longer along `grain` |
| `crumble` | plaster, earth, snow | Cells of `shardSize` throughout, round edge |
| `seams` | brick, planks | Apart where palette entries meet first (two shades of brick are enough) |

- **Your own pattern**: `registerFracturePattern(world, "slabs", fn)`. **A pattern is part of the
  simulation**: same answer for the same input on every machine, no clocks, no `rand()`, no state.
  Use the `FractureRandom` it is handed.
- **A `Strike`** is the full form: `at`, `energy` (or `reach`, metres, whatever the energy), `push`,
  `minimumBreak`, `shardSize`, `cause`, `striker`, `returnUnusedEnergy`. A voxel breaks only within
  the reach the energy has **in its own material**, so a steel bar in concrete breaks only nearer the
  strike. Guards (`PhysicsConfig::fracture`): search radius, longest reach, most shards and islands.
- **On impact is a system, not part of the step**: `installImpactFracture(app)` strikes each
  `breaksOnImpact` body at its contact, with the collision's energy times the share its hardest-pushed
  point took ([`impacts()`](../reference/runtime.md#runtimephysicsh); `BodyImpact`). Leave it out and
  write your own from `impacts()` and `fracture()`. What decides a hit:
  `PhysicsSettings::minImpactSpeed` (per body: `minImpactSpeed`), `minImpactEnergy` (per body), the
  material's `strength`, and `FractureLimits::minImpactBreak`, so a crate set down on stone chips
  nothing and pieces too small to break cannot cascade. A striker that breaks through something static
  gets back the energy the break didn't use (a boulder carries on through a wall). Plan:
  [impact-loads](../plans/impact-loads.md).
- **Shards are pieces**: each carries `Piece` and `breaksOnImpact` off, so debris doesn't shatter
  again on landing. Past `PhysicsConfig::maxPieces` (default 3000) the oldest are destroyed in
  `(madeAt, NetId, entity)` order, after `PiecesCleared` names them; remove `Piece` from one to keep
  it. `dustVoxels`: islands this small are removed outright.

### Cheap debris

`PhysicsConfig::debris` (`maxVoxels` > 0 turns it on): a piece of that many voxels or fewer from a
break of a body **without a `NetId`** is not a body. It is drawn as ever, but moved by a fall and a
bounce against static bodies (sphere to the world, sliding along up to three surfaces a step, rolling,
turning no faster than `maxAngularSpeed`), pushes nothing and is pushed by nothing, and goes after
`seconds` or past `maxCount`. A break makes most of its pieces this small, and making bodies of them was
most of a big break's cost. It is not in the solver, a query or a snapshot; a networked body's break
makes none. Plan: [cheap-debris](../plans/cheap-debris.md).

## Splitting and support

One pass for every voxel body, run after its voxels change: its face-connected islands are found across
all its chunks (grids too) and each stays or comes away. `BodyBroken` (`cause`: `Edit`, `Support`,
`Strike`, `Impact`, `Replicated`) says which, and is delivered **inside the step**, before the pieces'
bodies exist, so a handler can shape them in the step that made them.

- **A dynamic body cut in two** keeps its largest island; each other piece is a new entity with the
  parent's `RigidBody` (mass shared by voxel count), NetId and motion. A strike's shards always come
  away.
- **A static collider with `splits: true`** drops every island nothing holds up. What holds up is
  data: voxels of an **anchor material** (`PhysicsMaterial::anchor`), and with `anchorBase` the
  component's own y = 0 and below. A collider with neither drops nothing, and says so. Off by default:
  floating terrain falling is a choice. Islands join across edges and corners for a static collider (a
  diagonal brace is a staircase of voxels touching at corners); a moving body's islands are by faces.
- **A collider too big to look over whole** (a level's world) is searched from the change outward
  (`FractureLimits::localSupportVoxels`).
- **No world rollback.** `savePhysics` does not rewind across anything that makes or removes bodies,
  by design.
- **An edit that removes every voxel**: the body goes, the entity stays, `BodyEmptied` is sent.

## Voxel collision

What voxels collide as is `PhysicsConfig::collision.representation`:

| | Collides as | Notes |
|---|---|---|
| `Boxes` (default) | `utils::buildCollisionPieces`: voxels merged into boxes, never bigger than the voxels | A budget of pieces; a chunk over it falls back to its content box |
| `Tiles` | The blob's [tile table](../reference/formats/tile-table.md), read in place | Static ones as they are; moving ones weighed and merged by block once. No budget, no content-box fallback, no seams on a floor. |

`PROJV_COLLISION=tiles` runs anything (the unit suite, a demo) on `Tiles`. A body with too many chunks
for voxel ids in Jolt's sub-shape id names coarser cells instead (bricks), and a cell's boxes give one
contact between them. Plan: [voxel-collision-shape](../plans/voxel-collision-shape.md).

## Contacts and sensors

- **`ContactReports { only }`** on a body: it reports `ContactBegan` / `ContactEnded` (one per pair,
  however many parts touch, in an order every peer agrees on). `only` names collision groups: a
  pressure plate that counts players. `contacts(world)` and `touching(world, e)` read the same.
- **A sensor** is a body on the `Sensor` layer (`RigidBody` or a `StaticCollider`): nothing collides
  with it, and it reports what enters and leaves, for the layers in `senses`.

## Collision groups

`CollisionGroups { groups, ignores, family }` on a body, character or query filter
(`definePhysicsGroup(world, name)` makes the bits): two bodies pass through each other when either
ignores a group the other is in, and members of one **family** never collide (a break's shards).
Changed at run time without rebuilding a body; recorded, replayed and restored.

## Kinematic bodies

By default nothing stops a kinematic body: it goes where it is moved, through floors and walls.
`RigidBody::blockedByStatic` (`"stop"` or `"slide"`) ends a move where it touches static geometry, or
carries the rest along the surface. Fly one with `ConstantVelocity`, or set where it should go, rather
than adding to the `Transform` drawn (that is a step behind). `holdBody` holds a body still;
`driveBody` poses it each step (what a client does to a networked proxy).

## Editing bodies

```cpp
auto crater = runtime::voxelsInSphere(world, ground, blastCentre, 3.0f);   // removal ops
runtime::editVoxels(world, ground, crater);                                 // applies on the next step
```

- `editVoxels` goes through `utils::queueVoxelAdd/Remove` + `updateScene` at the start of the next
  fixed step, in order with other commands, so every peer edits on the same tick.
- Direct `utils` edits work too: physics notices the new content stamp at its next step, rebuilds
  the shape and splits what was cut loose.
- `voxelComponentOf(world, e)` is the component edits go to: the linked chunk or grid, or the one
  chunk inside a linked prefab.

## Determinism

| Property | How to use it |
|---|---|
| **Deterministic**: the same calls give bit-identical results | `physicsStateHash(world)` per tick; equal hashes, same state. Keyed by NetId, so peers' local ids don't matter. |
| **Restorable** | `savePhysics(world)` / `restorePhysics(world, bytes)`. Restore needs the same bodies and changes nothing when it refuses. |
| **Recordable** | `PhysicsSettings::record = true`, then `PhysicsWorld::recording()` and `PhysicsWorld::replay(log)`: says whether every step matched, and where it first diverged. |

Rules for anything that feeds a step: **no clocks, no thread counts, no unordered iteration**. The
worker thread count (`PhysicsSettings::threads`) is a speed setting only; results don't depend on
it. A Jolt upgrade is deliberate, with the physics tests run.

## Guards: input is checked, never asserted on

Jolt asserts (and stops a dev build) on input it cannot take. Everything that reaches it goes
through `PhysicsWorld`, which refuses, clamps or logs first.

| Setting (`PhysicsSettings`) | Default | Guards against |
|---|---|---|
| `maxBodies`, `maxBodyPairs`, `maxContactConstraints` | 65536, 262144, 131072 | Full tables: creation refused, dropped contacts counted in `PhysicsStats` |
| `worldMin`, `worldMax` | ±10000 | Bodies falling forever: reported as `BodyLeftWorld`, destroyed by default |
| `maxLinearSpeed`, `maxAngularSpeed` | 500 m/s, 47.1 rad/s | Runaway speeds (clamped every step) |
| `minMass`, `minGyrationRadius` | 0.01 kg, 0.05 m | Slivers the solver would spin up without limit |
| `restRadius`, `restAngle`, `restSeconds`, `restDamping` | 0.05 m, 0.1 rad, 1 s, 0.9 | Stacks rocking forever without sleeping |
| `velocitySteps`, `positionSteps` | 15, 2 | Soft stacks (Jolt's default is 10) |

A non-finite pose after a step is put back where it last was, at rest, logged, and counted in
`PhysicsStats::nonFiniteRecoveries` (always a bug).

`PhysicsConfig` (passed to `installPhysics`) carries `settings`, `collision`
(`utils::CollisionParams`), `destroyBodiesThatLeaveTheWorld`, `maxPieces`, `dustVoxels`, `debris` and
`fracture` (the guards on a strike).

## Events

| Event | Sent when |
|---|---|
| `PhysicsBodyRefused { entity, reason }` | An entity asked for a body and could not have one |
| `BodyLeftWorld { entity, position }` | A body went outside the world bounds (by default its entity is already destroyed) |
| `BodyBroken { entity, pieces, shards, cause, at, striker, energy, ... }` | A body came apart: an edit, lost support, a strike or an impact. In the step, before the pieces' bodies exist. |
| `PiecesCleared { entities }` | The piece budget is about to destroy these; take `Piece` off one to keep it |
| `ContactBegan`, `ContactEnded` | Two bodies began/stopped touching, where one asked (`ContactReports`) or is a sensor |
| `VoxelsEdited` | A body's voxels were edited (what a server replicates) |
| `BodyEmptied { entity }` | An edit removed every voxel a body was made from |
| `JointBroke { entity, bodyGone, force, torque }` | A joint exceeded `breakForce` / `breakTorque`, or lost a body |
| `SimulationSlow { droppedSteps }` | `Time` dropped fixed steps: the simulation is slower than real time |

Pieces and shards are new entities linked to new top-level components. Copy whatever else they
should carry (gameplay components) in the handler.

## The core on its own

`PhysicsWorld` works without the ECS:

```cpp
runtime::PhysicsWorld physics;
auto floor = physics.createBody({.shape = PhysicsShape::box({50, 1, 50}),
                                 .motion = MotionType::Static, .layer = PhysicsLayer::Static});
auto crate = physics.createBody({.shape = PhysicsShape::box({1, 1, 1}), .position = {0, 10, 0}});
for (int i = 0; i < 600; i++) physics.step(1.0f / 60.0f);
physics.bodyState(crate).position;     // resting on the floor
```

`runtime::physicsWorld(world)` reaches the one `installPhysics` made. Changing it directly bypasses
the tick ordering, so a replay won't see the change.

## See also

- [Add physics](../guides/add-physics.md): step by step, with every `entities.json` key.
- [`examples/16-sandbox`](../../examples/16-sandbox/) and
  [`examples/17-physics`](../../examples/17-physics/).

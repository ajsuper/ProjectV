# Physics: Jolt, voxel collision, ready for networking

Status: **approved**, 2026-10-09. **M0-M6 done** (2026-10-13); joints (2026-10-13), physics materials
(2026-10-14), fracture, the debris budget and support (2026-10-14/15) built, and fracture policy moved out
of the engine (2026-10-15, "Mechanism in the engine, policy in systems"). M7 (contours) waits on the
contour work. "One rule set for voxel bodies" below is done (2026-10-15). **Next: the rest of
"Roadmap", revised 2026-10-15 from a review of which games the system serves and where it gets in
their way.

## Decisions made

| # | Decision | Why |
|---|---|---|
| D1 | Jolt, pinned at a tagged release | Determinism, save/restore, CCD, internal-edge removal, compounds, character controller |
| D2 | Reliability over accuracy | A reliable, slightly wrong system can be built on; an unreliable one cannot |
| D3 | Static collision is **opt-in** (`physics.static`) | Installing physics changes nothing until a file asks for it; a forgotten floor is caught at once in development |
| D4 | Static grids do **not** split into islands unless the component opts in | Floating terrain falling off is a design choice, not a default |
| D5 | Collision geometry comes from one replaceable builder, and every gameplay normal comes from the shape | Contours and other voxel-modified geometry can replace boxes without gameplay changes |
| D6 | Server-authoritative networking is the target; the sim is deterministic and restorable anyway | Lockstep turns every determinism bug into a permanent split; determinism still buys small corrections, replays and rollback |
| D7 | Voxel geometry collides as volumes (boxes), never as triangle meshes | A body inside a volume is pushed out; inside a mesh it falls through |
| D8 | **Mechanism in the engine, policy in systems** (2026-10-15). The engine does what only it can (simulate, cut, report, guard) and asks for every choice a game would make differently; those choices are data on components and materials, or systems a game installs, leaves out or replaces -- built only on public calls. Behaviour is asked for, never assumed (D3, D4 are cases of it) | A game the engine's rules do not fit must still be buildable on it, without forking it. A rule written into the step is one every game inherits |


## Priorities, in order

1. **Reliable.** A body never falls through the world, never explodes, never goes NaN, never
   silently vanishes, never jitters forever. When something goes wrong, the system recovers and
   says so. A slightly wrong answer every time beats a right answer most of the time: games can
   be built on the first and not on the second.
2. **Ready for networking.** The simulation is a pure function of (state, tick inputs), it can be
   saved and restored, and nothing in it is named by a handle that only means something on one
   machine. No transport is built here; nothing built here has to be undone for one.
3. **Geometry can change.** Voxel contours (and other geometry that departs slightly from cubes)
   are coming. Collision shapes and surface normals come from one replaceable step. Swapping it
   changes how everything collides and what normal every query returns, with no gameplay changes.
4. Accuracy and speed, in that order, after the three above.

## Why Jolt

Jolt (github.com/jrouwe/JoltPhysics, MIT, used in Horizon Forbidden West and Death Stranding 2):

- **Determinism.** Built with `CROSS_PLATFORM_DETERMINISTIC` it gives the same results on every
  platform and compiler it supports, as long as API calls happen in the same order. Without that
  flag it is still deterministic on one binary.
- **`SaveState` / `RestoreState`** (`StateRecorder`): rollback, replays and desync checks come
  with it.
- **Continuous collision** (`EMotionQuality::LinearCast`) against tunnelling.
- **Enhanced internal-edge removal**, so things slide over seams between adjacent boxes without
  snagging. A box-built voxel world needs this.
- **Compound shapes, custom shapes, and a character controller** (`CharacterVirtual`).
- A multithreaded broadphase and solver, and sleeping.

Pin the newest tagged release at submodule time (v5.x as of writing), the way EnTT is pinned.

## Architecture

### A1. Three layers, one direction

```
utils/collision_geometry   voxels -> list of convex pieces    (no Jolt, no EnTT; pure, testable)
runtime/physics            pieces -> Jolt shapes; bodies; the step; commands; state   (Jolt + EnTT)
gameplay                   RigidBody components, commands, queries
```

**The decomposition lives in `utils/` and knows nothing about Jolt.** It turns a blob into
`CollisionPiece`s (a box, or later a convex polyhedron), each tagged with the voxel region it came
from. That is the seam the future geometry work replaces (A6). It is also the only part that reads
voxels, so it is tested exactly against the voxels (V1).

Jolt types stay inside `src/runtime/physics/`. The public API is a thin, opinionated wrapper:
commands in, poses and query results out. For advanced use it offers one escape hatch,
`physicsSystem()`, documented as bypassing the command queue's ordering guarantees.

### A2. The PhysicsWorld is the authority on dynamic bodies

`runtime::PhysicsWorld` lives in `world.ctx()`. It owns the `JPH::PhysicsSystem`, the shape cache,
the command queue and the tick counter.

Every body has exactly one writer of its pose, the same rule the Scene bridge already applies to
Transforms:

| motion | who moves it | how gameplay moves it |
|---|---|---|
| **Static** | nobody; the voxels it was built from | edit the voxels (A5) |
| **Kinematic** | gameplay, through Transform | patch Transform; physics follows with `MoveKinematic` the next tick, so contacts get the right velocity |
| **Dynamic** | the simulation | commands: impulse, force, set velocity, **teleport** (explicit, logged at trace) |

A patch to a dynamic body's Transform does **not** move it. In dev builds it logs a warning that
names the command to use instead. Silently letting two writers fight is how bodies end up
jittering, so the conflict is reported rather than resolved by guesswork.

### A3. Sim pose and presentation pose are different things

- The **sim pose** is the pose at the end of the last tick. Gameplay reads it with
  `runtime::bodyPose(world, e)`, and queries (raycasts, overlaps) run against it.
- The **presentation pose** is the sim pose blended between the last two ticks by
  `Time::fixedAlpha`. Physics writes it into `Transform` in PostUpdate, before the bridge syncs.
  This moves the sandbox's interpolation into the engine.

Later, a networked client also puts its error smoothing (easing a predicted body toward a server
correction) in this layer. The sim pose then snaps, and only the presentation glides.

### A4. Shapes

**Default representation (M1): merged boxes.**

- Each blob is decomposed by greedy box merging into a `JPH::StaticCompoundShape` of boxes, built
  in unit voxel space.
- Each body applies its scale with a `ScaledShape`. Shapes are therefore shared by every instance
  of a blob, just as geometry is (the prefab cache already guarantees the sharing), and a scale
  change swaps only the wrapper.
- Boxes have volume, so a body that ends up inside one is pushed back out. A triangle mesh would
  let it fall through, which is why there is no `MeshShape` for voxel geometry.
- Box convex radius is a fixed fraction of a voxel, so corners round off the same way at every
  scale.
- Enhanced internal-edge removal is on for every dynamic body.

**Grids:**

- Only for components that opt in (`physics.static`; see Components). Then one static body per
  grid component, its shape a `MutableCompoundShape` with one sub-shape per
  cell.
- An edit to a cell replaces that one sub-shape.
- If measurements show `MutableCompoundShape` updates are slow with thousands of cells, fall back
  to one static body per cell. Static bodies are cheap in Jolt's broadphase.

**Multi-chunk components** (a component subtree with several loose chunks): one compound whose
children are the chunks' shapes at their local transforms.

**Authored primitives:**

- `RigidBody.shape` can be `"voxels"` (the default), `"sphere"`, `"box"` or `"capsule"`, with a
  size fitted to the content bounds or set explicitly.
- A ball should collide as a sphere and a character as a capsule. Primitives are cheaper and
  steadier than any voxel approximation, and the sandbox's balls use one.

**Fallbacks, so every body always has a shape:**

- A decomposition that fails, or that exceeds the piece budget (configurable; default 1024 pieces
  per blob), falls back to the content-bounds box, with a warning.
- An empty blob has no shape. Its body is removed, not left as an invisible shapeless object.

**Shape cache:**

- Keyed by (blob `contentStamp`, representation params key).
- Blobs carry a `contentStamp` (see M1, as built). Every in-place geometry write goes through
  `markBlobContentChanged`, which also clears `contentBoundsValid`, so the two cannot drift apart.
- Shapes are refcounted (Jolt `Ref<Shape>`) and dropped when no body uses them.

**Mass:**

- Computed from the shape at a density (`RigidBody.density`, or `mass`, which sets density).
- Clamped to a minimum mass and a minimum inertia per axis, so a one-voxel sliver cannot become
  a near-zero-inertia body that the solver spins up.
- Mass ratios above 100:1 in one contact are logged once in dev builds. They are the classic
  stacking-instability source.

### A5. Edits take effect at tick boundaries

A voxel edit to something with a body:

- marks the body's shape stale;
- is rebuilt **at the start of the next tick**, never mid-step;
- the old shape stays in use until the new one is ready, so the body never has a missing or
  half-built shape.

M1 builds synchronously on the main thread, which is simple and deterministic. Moving the build to
a job is allowed later only if it keeps that guarantee: the swap still happens at a tick boundary,
on a tick chosen deterministically.

**Destruction (M5):**

- After an edit to a dynamic body, the edit runs a connectivity pass over the body's voxels
  (6-connected, on the brick map).
- Each disconnected island becomes its own component (`reserveComponentSlot` / `placeComponent`
  — row reuse makes this cheap now) with its own body.
- Each island inherits the parent's linear velocity plus the angular velocity's contribution at
  its centre of mass.
- Islands are ordered by their lowest voxel coordinate, so the split is deterministic.
- Static grids do not split (D4). A component opts in with `"splits": true` on its
  `physics.static`; the islands that come loose become dynamic bodies.

### A6. Collision geometry is replaceable, and normals come with it

`utils::CollisionRepresentation` is an id plus a builder:

```cpp
// The pieces a blob collides as, in unit voxel space, in a deterministic order.
std::vector<CollisionPiece> buildCollisionPieces(const GeometryBlob&, uint32_t resolution,
                                                 const CollisionParams&);
struct CollisionPiece {
    enum Kind { Box, ConvexHull } kind;
    core::vec3 min, max;              // Box
    std::vector<core::vec3> points;   // ConvexHull
    core::ivec3 voxelMin, voxelMax;   // the voxels it stands for: SubShapeID -> voxel, for
                                      // materials, damage and per-voxel friction
};
```

- **M1 ships `Boxes`.**
- **Contours become `ClippedVoxels`:** interior voxels stay merged boxes, and each surface voxel
  becomes a convex hull, its cube clipped by its contour planes (the up to three planes the
  surface-voxel prototype already fits).
- **Other "slightly voxel-modified" geometry** is another builder behind the same id.

Rules every representation keeps:

1. **Stays inside the voxels, never bigger:** collision ⊆ the union of solid voxel cubes. It may
   cut corners off; it may not add volume. Switching representation can therefore only let things
   settle a little lower; it cannot embed things that rested on the old shape. Tested (V1).
2. **Deterministic:** the same voxels and params give the same pieces in the same order, on every
   machine. Networked peers build shapes locally from replicated voxels and must agree.
3. **Normals come from the shape.**
   - Gameplay never derives a surface normal from voxel coordinates. Physics contacts, raycasts
     and shape casts all return the normal of the active representation.
   - `utils::pickVoxel` stays exact-voxel with face normals, because editing needs that, and is
     documented as the editor's tool, not gameplay's.
4. **One surface definition for render and collision:**
   - When contours land, the CPU function that evaluates a voxel's planes is what both the shape
     builder and the GPU upload read, so the drawn surface and the collided surface cannot drift
     apart.
   - A test samples points and compares the CPU's surface normal with the shader's
     (the render-vs-CPU pattern the colour lookup fix already used).
5. **Versioned:** the representation id and its params are in the shape cache key and in the
   network state hash. Two peers on different representations is a detected mismatch, not a
   mystery desync.

Choosing the representation:

- one project-wide default;
- a per-component override, in `physics.rigidbody` or `physics.static`;
- a per-body primitive (A4).

A custom Jolt shape that collides against the voxel tree directly is kept as a later option for
very large dynamic bodies. It is a performance lever only, and it plugs in behind the same seam.

### A7. Ready for networking

What the simulation guarantees from M0 on, whether or not anything is networked:

- **Ticks are the clock.** Physics advances only in FixedUpdate, one `Step` per tick at
  `fixedDelta`, numbered by `Time::fixedStep`. Nothing reads `delta`, the wall clock, the frame
  rate or render state.
- **All mutation is a tick-stamped command:** create body, destroy body, impulse, teleport, set
  kinematic target, voxel edit.
  - Commands are queued during Update and applied at the start of the tick, sorted by
    (tick, issuer, sequence).
  - Never in EnTT view order, which depends on the history of inserts and removes and differs
    between machines.
  - The command log is what a server receives from clients, and what a replay file contains.
- **Stable identity:**
  - Every networked body has a `NetId` (u32, assigned by the authority, carried in
    entities.json for authored bodies).
  - `Entity`, `ComponentHandle`, `ComponentRef` and `JPH::BodyID` are local and never leave the
    machine.
  - Bodies are created in NetId order within a tick, so Jolt's internal body order matches across
    peers.
- **Save / restore.**
  - `PhysicsWorld::saveState(StateRecorder&)` / `restoreState` wrap Jolt's, plus our side: the
    tick, pending commands, NetId map, shape-cache keys.
  - Restoring then re-simulating N ticks must give a bit-identical state to never having
    restored. That property is what rollback, replays and late-join snapshots all rest on.
- **State hash per tick** (dev builds, or on demand): a hash of positions, rotations and
  velocities in NetId order. Two runs or two peers compare hashes to find the first diverging tick.
- **Build:**
  - Jolt with `CROSS_PLATFORM_DETERMINISTIC=ON`.
  - No `-ffast-math` anywhere physics code is compiled.
  - Double precision off for now, but our wrapper uses `JPH::RVec3` for world positions, so
    turning on `JPH_DOUBLE_PRECISION` for large worlds is a rebuild, not a rewrite.

**The network model this prepares for** (a recommendation, not built here):

- **Server-authoritative.** Clients predict their own character and the bodies they touch; the
  server sends quantized snapshots of awake bodies, prioritized by relevance.
- **Other clients' bodies** are shown by snapshot interpolation.
- **Not lockstep.** In a physics-heavy voxel game with destruction, lockstep makes every
  determinism bug a permanent split. Server authority treats a determinism bug as a correction.
- **Determinism is still worth having:** it makes prediction corrections small, makes replays and
  desync tests possible, and keeps rollback open.

### A8. Reliability guards

Each guard is cheap, always on (not just debug), and counted in `PhysicsStats`, which the sandbox
shows and tests assert on:

- **Sanity pass after every step:**
  - Any body with a NaN or infinite pose or velocity, or speed above a hard cap, is restored to
    its pose from the previous tick with zero velocity, put to sleep, and logged once with its
    NetId.
  - Any body below the kill plane or outside world bounds is reported with an event
    (`BodyLeftWorld`), not deleted silently. What happens to it is the game's call; the default
    handler destroys it.
- **Tunnelling:**
  - Bodies smaller than a few voxels, or faster than a size-relative threshold, get
    `LinearCast` automatically.
  - Static geometry has volume (A4).
  - A test fires small, fast objects at one-voxel walls (V3).
- **Capacity:**
  - Max bodies, body pairs and contact constraints are configured with 2x headroom over the
    expected load.
  - Jolt drops contacts silently when full, so every step checks the counts. Crossing 80% warns
    once; a drop is an error with the counts in the message.
- **Spiral of death:** Time already caps fixed steps per frame at 8 and counts the dropped ones.
  Physics reports dropped steps as "simulation running slow", so slow motion is visible rather
  than mysterious.
- **Sleeping** is on, with Jolt's defaults first. A stacking test (V3) says whether they are
  right for voxel-sized bodies.
- **Solver settings** stay at Jolt's defaults (10 velocity, 2 position iterations) until a test
  shows otherwise. Changes are recorded with the test that motivated them.
- **Asserts:** Jolt asserts on in the `dev` preset, off in `release`. The `release` build still
  runs the sanity pass.
- **No surprises from the bridge:**
  - A body whose component is deleted (`ComponentDestroyed`) is destroyed at the next tick
    boundary.
  - A component whose body is gone keeps rendering where it was.
  - The `ComponentRef` in the link means a reused row is never driven by a stale body.

## Components (authored in entities.json)

```json
"physics.rigidbody": { "v": 1, "motion": "dynamic", "shape": "voxels", "density": 1.0,
                       "friction": 0.6, "restitution": 0.2, "linearDamping": 0.05,
                       "angularDamping": 0.05, "gravityFactor": 1.0, "layer": "moving",
                       "quality": "auto", "netId": 0 }
```

- `RigidBody` is the authored component. On construct, physics queues a create command; the body
  appears at the next tick.
- `PhysicsBody` (runtime-only) holds the `BodyID`, the shape-cache key and the previous and
  current sim poses for interpolation. It is never saved.
- **Static collision is opt-in** (D3): a `physics.static` component on an entity linked to the
  grid (or any component) makes it solid. Nothing collides that was not asked to, so installing
  physics changes nothing in an existing scene until a file says so, and a floor that was
  forgotten is obvious the first time something falls through it.

  ```json
  "physics.static": { "v": 1, "shape": "voxels", "friction": 0.8, "restitution": 0.1,
                      "layer": "static", "splits": false }
  ```
- **Layers:** a short fixed list (`static`, `moving`, `debris`, `sensor`, `character`) with a
  collision matrix in code; debris ignores debris. Authored by name. The matrix is the coarse
  broadphase cut, not a game's filtering: a fixed matrix is policy (it decided that shards could
  not collide with each other, which a game may not want), so finer filtering is data -- collision
  groups, Roadmap R4.

## Queries

`runtime::castRay`, `castShape`, `overlap`:

- They run against the sim pose.
- Each returns entity, component, `NetId`, the voxel the hit sub-shape stands for, its material
  slot, position, distance, and **the representation's normal** (A6.3).
- Every query is a Jolt query, so it uses Jolt's broadphase. The loose BVH stays the renderer's
  and the editor picker's.

## Joints (added 2026-10-13, built)

- `PhysicsWorld::createJoint` over Jolt constraints: Fixed, Point, Hinge (limits, friction, motor),
  Slider (limits, friction, motor), Distance (range or as made, optional spring). A joint joins two
  bodies or a body and the world, placed in world space as they are. Jolt's body 1 is `b`, so a
  hinge angle, slider offset and motor velocity are `a` relative to `b`.
- Breakable by force and torque, checked after each step from the constraint's impulse. The
  impulse beyond what it could hold in the breaking step is given back, so what broke it carries
  on (a rope snapped by a yank does not stop the yank). `takeBrokenJoints`.
- Joints go with their bodies (destroyBody removes them). Joints get their own solver iteration
  counts (30 velocity, 6 position): a ten-link chain stretched 3% with the contact counts, under 1%
  with these.
- Recorded, snapshotted (version 4: the live set must match), hashed.
- Runtime: `Joint` (`physics.joint`, bodies by NetId, 0 for the world) on its own entity, made once
  both bodies exist; `PhysicsJoint`; `JointBroke` (broke, or a body went); `setJointMotor` as a
  stamped command; `connectBodies`, `jointState`.
- Tests: a pendulum's period against 2 pi sqrt(L/g), a hinge's limits and axis, motors, a slider's
  stop, a weld, a rope and a spring's stretch against mg/k, breaking at the right force, joints going
  with bodies, a chain's determinism, rewind and replay, the runtime path and entities.json.

- Added the same day: motors in Position mode (servos: `motorMode`, `motorTarget`,
  `motorFrequency`), and `collideConnected` (default off, as in most engines): bodies a joint joins
  do not collide, through a Jolt group filter keyed by body index; pairs return to colliding when
  the joint goes or breaks. A running motor keeps its bodies awake (a jammed windmill slept for
  good otherwise). Demo 7 is a car built from these.

## Physics materials (decided 2026-10-13, built 2026-10-14)

Linked through the palette: every voxel's material slot already names a palette entry, so a
palette entry names a physics material from a project-wide library (density, friction, restitution;
later strength and sound). Box merging stops at material boundaries, so each piece has one material:
mass, centre of mass and inertia from per-piece density, and friction/restitution per contact from
the two sub-shapes' materials through a contact listener. Voxels with none fall back to the body's
RigidBody values. Material interning must include the physics material (not colour alone); shape
cache keys include the material assignment; the library joins the network compatibility check.


As built (86067ba, demo in 6bcd893):
- The library: `runtime::definePhysicsMaterial` / `loadPhysicsMaterials` (`{"v":1,"materials":[{name,
  density, friction, restitution}]}`), indices in definition order, in both state hashes.
- A palette entry names one by `Material::physics` ("physics" in compose.json), outside the four GPU
  words. Several entries can name one (three colours of granite merge into the same boxes); entries
  alike in colour can name different ones (interning matches on the physics name too). The slot
  limit stays 255 per component: one slot per (look, material) pair a component actually uses.
- `RigidBody::material` / `StaticCollider::material` is the component default: primitives and
  voxels whose entry names none. Empty: the fields, as before.
- Boxes stop at material changes; pieces carry the material; per-piece density gives mass, centre of
  mass and inertia; a contact listener combines each side's friction (geometric mean) and
  restitution (max), so a world with no materials is unchanged bit for bit.
- Bodies rebuild when an entry's material or a library entry changes. Log version 4.
- Not yet: a second per-voxel byte if components ever need many looks times many materials.

## Fracture (decided and built 2026-10-14)

A strike -- `runtime::fracture(world, entity, at, energy, push)`, applied at the next step in order
with voxel edits -- breaks a voxel body round a point. The voxel nearest the point (within three) is
what was struck; its material's `strength` (J per m^3 broken) sets the reach, the radius whose sphere
holds energy / strength. Which voxels go together is the material's `fracture` pattern; everything
else is the engine's. Decided (user's call): not one fixed pattern, and not free-form either --
patterns are code, registered by name, all through one interface.

- **The interface** (`runtime/fracture.h`): a pattern gets the voxels within 1.5 x reach (position,
  palette slot, material, breakable), the strike, reach, shard size and grain in voxels, and a
  seeded `FractureRandom`; it returns a shard number or -1 (stays) per voxel. It is simulation code:
  same input, same answer, on every machine (no clocks, rand() or unordered iteration). The seed is
  the body's NetId, the tick and the strike.
- **Built in**, registered by installPhysics: `shatter` (Voronoi, small at the strike growing to
  `shardSize`, a ring of stay seeds beyond the reach for a jagged edge), `splinter` (the same, cells
  three times longer along `grain`), `crumble` (even cells, round edge at the reach), `seams` (runs
  of one palette entry come away whole -- a brick, a plank -- crumbled further only when much bigger
  than a shard).
- **The engine's rules, whatever a pattern says**: a voxel whose material has no strength stays; a
  shard is cut into its face-connected parts and halved until it fits a 64-voxel chunk; at most 512
  shards. Each shard is a new top-level chunk with the body's palette (voxels keep their entries:
  `PendingVoxelOp::slot`), placed where its voxels were, a dynamic body moving as that part of the body
  did plus `push` fading to nothing at the edge; a NetId if the body had one. Static colliders break
  too. `BodyFractured {entity, shards, at}`. What is left rebuilds, and splits as any edit's does.
- Material fields `strength` (0: never breaks), `fracture` (default "shatter"), `shardSize` (m),
  `grain`; in the library file and the state hash. Reach capped at 64 voxels.
- Tests (`test_physics_fracture.cpp`): voxels conserved and materials kept, reach against energy,
  steel in a stone wall stays, a strike on steel breaks nothing, determinism (shard sizes and state
  hash), a dynamic body's shards keep its motion, a registered pattern decides, a wrong-sized answer
  breaks nothing, fractureShards' cuts, every built-in within its reach and repeatable, splinters
  along the grain, the library fields.
- **On impact** (user's call, 2026-10-14: the engine breaks things, not the demos). `PhysicsWorld::
  impacts()`: contacts that began in the step closing faster than `minImpactSpeed` (1 m/s), noted by
  the contact listener on the solver's threads and ordered after the step -- one per pair, the
  fastest, by (a, b) -- with the closing speed, the energy the collision can take out (half the
  reduced mass times speed^2) and both velocities from before. The runtime strikes each voxel body
  with `breaksOnImpact` (RigidBody, StaticCollider; saved when off) that has a breakable material,
  at the next step, with that energy. Rules: an impact must break out at least the struck
  material's shardSize (a crate set down on stone does not chip it); shards come with
  `breaksOnImpact` off (debris does not shatter again on landing); the shards go on at the closing
  speed, fading to the edge; and when the struck body does not move, the striker gets back the energy
  the break did not use (strength x volume broken), along its old velocity. `BodyFractured` gained
  `striker` and `energy`.
  - Impact speeds come from every awake body's velocity snapshotted before the step, not read in
    the contact callback: Jolt applies gravity on other threads while it finds contacts, so a
    velocity read there depends on timing. Test: the same impact fractures at 0, 4 and 12 threads.
  - Found: thirty blasts in demo 2 (~50k shards) filled a contact buffer; Jolt's dev-build assert on
    that stopped the program. It is now let through (step() reports it loudly and carries on, as a
    release build does); every other Jolt assert stays fatal. The real fix is a debris budget.
  - Found: the shards spawn tiled, touching, so a big break is one jammed mass of rubble. A 2 t boulder
    always gets through a plaster wall's crumbs; in stone, brick and planks it sometimes stops in
    the plug and sometimes ploughs on, depending on how the rubble lies.
- Demo 9: four walls, four materials and patterns (ashlar shatter, brick seams, planks splinter,
  plaster crumble), broken by the boulder hitting them; the demo has no fracture code, and its
  self-test fires at all four. Demo 2: bricks of "fired brick", the blast is `runtime::fracture` on
  each brick near it, and thrown bricks break where they land hard.
- Not yet: patterns that read authored crack lines (an attachment), sounds on impacts, merging
  settled debris back into static geometry.

## Debris and support (built 2026-10-15)

- **Debris budget.** Every shard carries `Debris {madeAt}`; past `PhysicsConfig::maxDebris` (3000)
  the oldest go at the step that made the newest, in (madeAt, NetId, entity) order, their bodies at
  once. Remove `Debris` to keep a shard. Found with demo 2's `DEMOS_BLASTS=30`: 8,233 bodies, Jolt's
  pair and contact buffers full (and so a session that no longer replayed), frames up to 194 ms.
- **Capacities** for fracture: shards are made packed, each touching many others. maxBodyPairs
  262,144 and maxContactConstraints 131,072 (were 65,536 and 32,768).
- **Cost of a blast.** A step's strikes are applied in two halves around one updateScene (it was two
  per strike, ~0.35 ms each whatever the edit). Jolt's broadphase is rebuilt (OptimizeBroadPhase)
  once 256 bodies have come since the last time -- a count, so replays do the same -- since a
  thousand shards added one by one left it lopsided: the step after a blast took 36 ms, now 17.
  Thirty blasts now: frames 5.6 ms on average, 73 ms at worst, no step dropped, ~3,600 bodies.
- **Impact pushes** blend velocities: a shard takes the striker's pre-impact motion at the contact,
  fading to the struck body's own at the edge. (It was the striker's closing speed along the normal,
  which threw a brick that landed on a floor upward at the speed it fell -- the floor counted as the
  striker.)
- **Support.** `StaticCollider::splits` (saved since M5, now acted on; D4 holds -- opt-in). After
  its voxels change, every face-connected island, across all its chunks, that no longer reaches the
  component's own y = 0 falls as a dynamic body of its voxels and palette entries (BodySplit), of the
  collider's friction, restitution and material. Worked out in a box over the component (at most
  2^24 cells, else logged and skipped); at most 512 pieces. Not yet: anchors other than y <= 0
  (voxels marked as anchors, touching another static body).
- Tests: the budget (oldest first, a kept shard stays, bodies go at once); support (a hole that cuts
  nothing loose, a slot right across drops the top by the slot's height, made of what it was, with
  a NetId; a collider that does not split; a strike through a pillar brings the top down).
- Demo 9's walls split: the bricks the seams pattern leaves hanging in their mortar fall, and a
  wall cut through drops what is above.
## Mechanism in the engine, policy in systems (decided and built 2026-10-15)

A review of fracture as built: the cutting, the rules it holds every pattern to, the patterns and
impact reporting belong in the engine; several decisions a game should own were made there instead.
The engine's pattern is mechanism in the engine and policy in systems the game can replace
(installPhysics adds systems; a joint's breakForce is the game's number, not a rule the engine
picks).

What oversteps:
- Breaking on impact is on by default: `breaksOnImpact` defaults to true, so giving a material a
  strength makes every body of it break when hit -- against "nothing collides that was not asked
  to", D3 and D4.
- Impact policy is hard-coded in the step: an impact must break at least shardSize; shards never
  break again; shards blend the striker's motion; the striker gets back the energy a fixed body's
  break did not use -- a velocity written on a body the game never asked the engine to touch.
- Debris clearing destroys entities by default and says nothing (destroyBodiesThatLeaveTheWorld,
  the precedent, sends BodyLeftWorld and has a flag).
- Support hard-codes "ground is the component's y <= 0": a game's definition of anchored.

To do, in order:
1. **Opt-in.** `breaksOnImpact` defaults to false (RigidBody and StaticCollider), as `splits` does.
   Shards are new bodies, so "shards never break again" goes with it.
   *Done 2026-10-15:* defaults false, saved only when on; pieces that fall when support goes take
   the collider's. Demos 2 (bricks), 9 (walls) and 11 (overhangs) opt in. The shard rule moves with
   item 2, into its config.
2. **Impact fracture as a system.** Move the impact -> fracture glue out of the physics step into
   `installImpactFracture(app, config)`, built only on public calls: `runtime::fracture` and a new
   entity-level `runtime::impacts(world)` (entities, not body ids) -- which is also the contact event
   games want for sounds and damage (item 3). Its config, today's values as defaults: the minimum
   break (shardSize), whether the striker gets back unused energy, how shards take the striker's
   motion, whether shards break on impact.
   *Done 2026-10-15:* `src/runtime/impact_fracture.cpp`, "physics: impact fracture" in FixedUpdate
   after "physics", on public calls only: `runtime::impacts(world)` (`BodyImpact`, entities), a
   public `canBreak`, and `runtime::Strike` -- the options a strike takes (energy or `reach` in m,
   push, `bodyVelocity`, `minimumBreak` in shard sizes, `striker`, `returnUnusedEnergy`,
   `shardsBreakOnImpact`). `ImpactFractureConfig` holds today's values as defaults. Without the
   system nothing breaks on impact; step 7a is gone. Shards copy the body (a static collider's: its
   breaksOnImpact too) unless the strike says; demo 2's blast and demo 11's chips say no. Item 5's
   reach overload is `fractureWithin` (the energy overload has the same signature).
3. **Debris event.** `DebrisCleared {entities}`, sent before they are destroyed, so a game can keep
   or replace them. The budget stays on by default (3000): debris exists only where a game asked
   for fracture, and unbounded debris is a reliability failure, not only slowness -- thirty blasts
   filled Jolt's contact buffers and the session no longer replayed.
   *Done 2026-10-15:* `DebrisCleared {entities}`, oldest first, delivered with `Events::trigger`
   (a queued event would arrive after they were gone). A handler that takes `Debris` off one keeps
   it; every entity still with Debris afterwards is destroyed. The newest named may not have bodies
   yet: the budget is kept in the step that made them.
4. **Anchors as data.** An `anchor` field on the physics material: a bedrock or foundation material
   holds up what touches it. Data rather than a callback, since StaticCollider is saved and
   callbacks are neither saved nor the same on every peer. "y <= 0" stays only as an opt-in fallback
   on the collider.
   *Done 2026-10-15:* `PhysicsMaterial::anchor` ("anchor" in the library file, in the state hash):
   an island with a voxel of an anchor material stands. `StaticCollider::anchorBase` (saved when on)
   is the old y <= 0 rule, now asked for; demos 9 and 11 ask. A collider that splits with neither
   drops nothing and warns once (everything falling is a setup mistake, not a collapse). Still
   within one collider: touching another static body's anchor does not hold yet.
5. **Limits and a reach overload.** The 3-voxel search for the struck voxel, the 64-voxel reach cap
   and the 512-shard cap are guards, like maxLinearSpeed: configurable (PhysicsConfig), still
   enforced. The sphere reach (energy / strength) is what `strength` means, not tuning; instead,
   `fracture(world, entity, at, reach)` takes a radius in metres for games with their own model.
   *Done 2026-10-15:* `PhysicsConfig::fracture` (`FractureLimits`: searchVoxels 3, maxReachVoxels
   64, maxShards 512), clamped by installPhysics to 0.5..16, 1..256 and 1..4096 and warned about,
   never off. The reach overload is `fractureWithin(world, entity, at, radius, push)` and
   `Strike::reach` (see item 2).

Also: document that shards copy the parent's whole RigidBody (damping, gravity factor, layer).
Demos 2 and 9 opt in explicitly; tests follow. *Done:* Strike::shardsBreakOnImpact says so; demos
2, 9 and 11 opt in.

Found while doing it: a click's chip in demo 11 breaks the wall but its shards stay in the hole
(Roadmap R1).

## One rule set for voxel bodies (decided and built 2026-10-15)

Playing demo 11 showed the breaking rules had grown a case per kind of body: shards (fracture's,
with rules of their own), pieces that fall when support goes, debris (a tag and a budget), anchors,
and bodies that move, each split by different code or not at all. The visible result: a fallen
piece cut in two stays one body moving together, because it is a grid and only single chunks
split; and debris could not break again. User's call: shards are too engine-incorporated, the kinds
are too separate, and support-style rules should apply to everything.

They are all one thing: voxels that were part of one body and now are a body of their own. So:

1. **One pass after every change.** Whenever a voxel body's voxels change, for whatever reason (an
   edit, a strike, an impact), its face-connected islands are found, across all its chunks, and
   each stays or becomes a body of its own:
   - a moving body: every island but the largest becomes its own body (nothing holds a moving
     body together but being connected);
   - a static collider that splits: islands with an anchor stay; the rest fall.
   This replaces support (dropUnsupported), the moving split (splitCutPieces, single chunks only)
   and fracture's own piece-making. It needs grids and multi-chunk bodies to split (was R10).
   *Done 2026-10-15, for edits and support:* `splitIslands` (physics.cpp) is the one pass, run on
   every voxel body whose voxels changed; splitCutPieces and dropUnsupported are gone. It works on
   the voxels of all a body's chunks (a chunk, a grid's cells, a one-chunk prefab), so a moving grid
   splits across its cells (test: "a moving grid body splits across its cells"). A piece is a new
   top-level chunk cornered at its lowest voxel (growing into a grid when it outgrows 16), with the
   palette but not the attachments; at most 512 pieces a pass, as support had. Still not split: an
   asset of several chunks (no shared voxel lattice), and a kinematic body. Fracture still makes its
   own shards: item 2. *Item 2 done the same day:* fracture's piece-making is gone too.
2. **A strike is an edit.** A fracture only decides which voxels go together -- the pattern's job,
   still pluggable -- and cuts them apart; the groups become bodies through the same pass, the way
   islands do. No shard-only rules.
   *Done 2026-10-15:* a strike (breakBody) only runs the pattern and fractureShards, and leaves the
   groups on `PhysicsRuntime::breaking`; the pass cuts them with the body's other islands, so the
   top a strike cuts loose comes away in the strike's own event. Voxels of two groups do not join.
   A second strike on a body in one step runs the pass for the first before it is worked out (it
   sees the cut), as a plain edit before a strike is applied first. A body too big to look over
   whole (2^24 cells), or one whose rest always stays (a static collider that does not split, a
   kinematic body), reads only the shards. The push, `bodyVelocity` and the striker's unused energy
   happen in the pass, for shards only.
3. **Every piece takes after its parent.** A piece of a moving body copies its RigidBody; a piece
   of a static collider gets one from the collider's friction, restitution, material and
   breaksOnImpact. Debris breaks on impact if what it came from did (user's call: it should).
   `Strike::shardsBreakOnImpact` and `ImpactFractureConfig::shardsBreakOnImpact` go.
   *Done 2026-10-15:* both gone; a kinematic body's pieces copy its RigidBody as dynamic. Demos 2,
   9 and 11 no longer turn it off. A handler of the event can (test: "a strike by reach...").
4. **One kind of piece, one event.** `Debris` becomes `Piece {madeAt}`, on everything a break made:
   shards and fallen pieces alike. `BodyFractured` and `BodySplit` become one event -- what broke,
   its pieces, and why (strike, impact, edit, support) -- delivered at once, before the pieces'
   bodies are made, so a game can shape them in the step that made them (was R5).
   *Done 2026-10-15:* `Piece {madeAt}`, `PiecesCleared`, and `BodyBroken {entity, pieces, shards,
   cause, at, striker, energy}` with `BreakCause` Edit, Support, Strike or Impact (`Strike::cause`;
   installImpactFracture sets Impact). Delivered with `Events::trigger` in the step; a piece has its
   entity, link, RigidBody and NetId but no body yet, so a RigidBody patch costs nothing and
   setVelocity (stamped for this step, applied after bodies are made) sets how it starts. A strike's
   shards come first in `pieces`, `shards` of them, so a game can tell them from what fell.
5. **Guards, the same for every piece, as settings** (PhysicsConfig):
   - the piece budget (today's debris budget, over all pieces);
   - the smallest piece: an island of fewer voxels than this is removed as dust (what demo 11 does
     by hand today);
   - the least impact break (today's ImpactFractureConfig::minimumBreak: a hit must break out at
     least a shard's worth). With the smallest piece, this is what keeps debris that breaks on
     impact from cascading: every break makes smaller pieces, and one below a shard's size cannot
     be broken by a hit.
   *Done 2026-10-15:* `PhysicsConfig::maxPieces` (3000), `PhysicsConfig::dustVoxels` (0: no dust;
   an island of that many voxels or fewer is cut and nothing made of it), and
   `FractureLimits::minImpactBreak` (1 shard size, clamped 0.5..16), applied to strikes whose cause
   is Impact: the reach must be at least that, and a body that fits in a cube of it is not broken by
   a hit at all -- which is what actually stops the cascade (the reach test alone does not: a piece
   smaller than the reach is broken whole). `ImpactFractureConfig::minimumBreak` is gone into it;
   `Strike::minimumBreak` stays, for a game's own knocks. `Strike::shardSize` built too.
   Tests: one event for a struck moving body (shards and the end it left), pieces taking after a
   moving body, dust, Strike::shardSize, the least impact break.

What stays: RigidBody and StaticCollider stay two components (Jolt's static and moving, and D3);
anchors stay data (anchor materials, anchorBase) and only matter to static colliders; static
splitting stays opt-in (D4); patterns stay pluggable. One addition: `Strike::shardSize`, the size
pieces break to for this strike instead of the material's, so a game can break a small thing finer
than a big one (a pickaxe that crumbles what it hits).

Costs: grid splitting first; demos 2 and 9 make more pieces once debris breaks again (bounded by
the guards; their self-test thresholds may move); `Debris`, `BodyFractured` and `BodySplit` are
renamed or merged, with the tests and demos.

Order: grid and multi-chunk splitting; the one pass; pieces and the one event; the guards; then
demo 11's pickaxe on top -- a shard size scaled to what it hits, crumbs as dust, and a moment
without friction for thrown pieces (their friction otherwise holds them flush in their sockets).
*Done 2026-10-15:* the click breaks to 0.35 m (`Strike::shardSize`); the demos set dustVoxels 5
(was the demo's own rule); a click's shards are made frictionless in the BodyBroken handler, before
their bodies exist, and thrown once they do. F became a 3 MJ demolition blow, and demo 11 gained a
composite wall (concrete frame with rebar, brick in mortar, pine, glass; README), which the
self-test blasts: thousands of voxels out, every rebar voxel stays.

Found on the way: demo 9's "nothing fell through the floor" check counted a piece that went over the
floor's edge, struck the floor's side and drifted back under it as it fell (about one run in five,
now that pieces break again in the air). Diagnosed by recording where each leaver first went below
the floor's top: beyond the edge, every time. The demo now ignores bodies seen below the top past
the edge; a real fall-through still counts.

## Roadmap (revised 2026-10-15)

From a review of what games would use this for and where it serves or blocks them. **Status:** a
game that wants no destruction gets none; one that wants its own rules can build them on public
calls. What still gets in a game's way, rather than merely not being built: the fixed layer
matrix, events that arrive a step after what they report, fracture choices still fixed in the
engine, and rollback that cannot cross a break.

Ordered by the priorities: what breaks reliability first, then networking, then what blocks a game
(D8), then what is merely missing. Each item says what the engine provides (mechanism) and what
it leaves to data or systems (policy); every default leaves behaviour as it is.

**Reliability**

- **R1. Shards that stay where they broke.** *Understood 2026-10-15:* not an engine fault. Cut from
  one solid, the shards sit flush in their sockets and friction holds them (set to 6 m/s, none
  moved more than 2 cm; with friction off, nearly all flew; a rebuild alone freed none). The demo
  handles it: a click throws its pieces, frictionless for a moment. Found on the way and fixed: a
  rebuild after a RigidBody change dropped the body's velocity (f3aeb09), and a collider whose
  anchors were all cut away was left floating (5c1ed8c).
- **R2. Sleep that freezes bodies mid-fall.** ("What the demos showed" 4.) Bodies touching only
  static geometry sleep while still falling. A body never stops somewhere it could not rest.
- **R3. Rollback across a break.** `savePhysics` needs the same bodies to exist at restore, and a
  fracture, split or debris clearing changes them, so a rollback game with destruction cannot
  rewind past one (D6). Mechanism: a snapshot that records body creation and removal since it was
  taken, so restore can undo them -- or a documented alternative a game can use. And the
  cross-machine determinism test (CROSS_PLATFORM_DETERMINISTIC, two builds, one recording).

**What gets in a game's way** (D8)

- **R4. Collision groups as data.** A body names groups (strings, like materials) and which groups
  it ignores; saved with RigidBody/StaticCollider, compared by NetId order so peers agree. Layers
  stay the coarse cut. A strike can put its shards in a group of their own. Default: no groups,
  nothing changes. Replaces the need for blanket rules like "debris ignores debris".
- **R5. Shaping what a step made, in that step.** *Folded into "One rule set", item 4; done.* `BodyFractured` and `BodySplit` delivered at
  once, as DebrisCleared is, before the new bodies are made, so a handler can change their
  RigidBody (layer, group, damping) and set the velocity they start with (setVelocity on a body
  not yet made sets its starting velocity). A Strike also takes how the push spreads (fading to the
  edge, as now, or the same for every shard). Default: as now.
- **R6. Contact and sensor events, asked for per body.** `ContactBegan` / `ContactEnded` for bodies
  that ask (a component, saved), and sensor bodies that report what enters and leaves, with the
  layers they sense chosen in data. In NetId order after the step. `impacts()` stays for hard hits.
- **R7. Kinematic bodies that collide with static geometry**, asked for per body. ("What the demos
  showed" 7.)
- **R8. The last constants a game might set:** the 512-piece cap on support splits, joining
  maxShards in PhysicsConfig::fracture; minImpactSpeed per body (a glass cup and a boulder want
  different thresholds), the world setting as its default.

**Missing, buildable on the engine meanwhile**

- **R9. Support across colliders.** A collider standing on another's anchor material is held up
  by it. Data, as anchors are.
- **R10. Dynamic grids and multi-chunk bodies split and break.** *Promoted: the first step of "One
  rule set"; grids split (2026-10-15), assets of several chunks do not.* A mechanism gap: today shards of
  dynamic bodies are capped at one chunk.
- **R11. Joints.** A true lock, a gear joint (two hinges at a ratio), a mass-ratio warning; ropes as
  a system over Distance joints rather than an engine primitive. ("What the demos showed" 6.)
- **R12. Scale.** Start-asleep for authored stacks; a profile of a settling pyramid. (8.)
- **R13. A debug view** of shapes, contacts, joints and sleep. (9.)
- **R14. Settled debris merged back into static geometry**, as a system a game installs (policy:
  when debris counts as settled, and whether it merges, are the game's).

**Not the engine's** (D8): damage and hit points, impact sounds, a grab spring, what a click
breaks. Games, or optional systems like installImpactFracture, built on public calls. A system
the engine ships is an example and a default, never the only way.

**Waiting:** M7, contours (the surface-voxels work); voxel gears wait on it too (5).

## How it fits the rest of the engine

Physics adds one module, `runtime/physics`, and one engine change: system ordering within a stage
(I1). Everything else plugs into what exists.

**I1. Application and Time.**
- `runtime::installPhysics(app)` adds two systems, the way `installSceneBridge` adds one:
  - "physics: step" in FixedUpdate, which applies this tick's commands, rebuilds stale shapes,
    runs `Step` and the sanity pass.
  - "physics: present" in PostUpdate, which writes interpolated poses into `Transform`.
- **The rule for gameplay:** anything issued during tick N, from Update or from a FixedUpdate
  system, applies at tick N+1. That holds whatever order systems were added in.
- "physics: present" must run before the bridge's PostUpdate system (`"scene bridge"`). Today a stage runs systems in the
  order they were added, so the order would depend on install order. The engine change:
  `addSystem` takes an optional `before` / `after` system name, and `installPhysics` asks for
  `before = "scene bridge"`. A small, general addition, with a unit test.
- `Time::scale = 0` runs no FixedUpdate, so pause and slow motion apply to physics with no extra
  code. Dropped fixed steps are reported (A8).

**I2. The Scene bridge.** Physics never touches the Scene directly; it goes through the bridge:
- It writes `Transform` with `patch`.
- The bridge then calls `setComponentTransform`, which rebakes the headers, which the renderer
  draws.
- Only awake bodies are written, so a sleeping pile costs nothing per frame.
- A body needs its entity linked to a component (`VoxelComponent`):
  - Dynamic bodies need a Root link, because their pose is a world pose.
  - Part links (a door in a house) can be kinematic.
  - A `RigidBody` that breaks these rules is refused with a log, the way `linkComponent` refuses
    bad links.
- `ComponentDestroyed` destroys the body at the next tick.
- `Transform::scale` (uniform) becomes the `ScaledShape`'s scale.

**I3. Authored entities and prefabs.**
- `physics.rigidbody` and `physics.static` are ordinary `ComponentTraits` components. They are
  saved in entities.json, built by their `on_construct` signals, and round-trip through
  `saveEntities`.
- A prefab whose entities.json carries `physics.rigidbody` spawns with a body from
  `instantiatePrefab`, with no extra call.
- The sandbox's crate becomes: compose.json (voxels), plus entities.json saying "dynamic,
  voxels, density 1".

**I4. Geometry pool, prefab cache and slot reuse.**
- The shape cache is keyed by blob, and the prefab cache makes instances share blobs. So 300
  crates are one Jolt shape.
- Editing one crate forks its blob (copy-on-write), which gives it a new key and its own shape.
  The other 299 are untouched. Physics adds no sharing logic; it follows the pool's.
- `PhysicsBody` holds a `ComponentRef`, so a reused row is never driven by an old body.
- Destruction makes new components through `reserveComponentSlot` / `placeComponent`, which row
  reuse made cheap under churn.

**I5. Voxel edits.**
- They stay what they are: `queueVoxelAdd` / `queueVoxelRemove` and then `updateScene`.
  - Physics notices changed blobs at the start of the next tick, by `contentVersion`.
  - Grid edits change only the touched cells' sub-shapes.
  - An edit that overflows a chunk and converts its component into a grid (`updateScene` does
    this) is handled: the builder treats a grid component as a compound of cells whether it is
    static or dynamic.
- **For networked games**, edits go through a physics edit command instead. It queues and runs
  `updateScene` at the start of the tick it is stamped with, so every peer edits on the same tick.
  Direct edits keep working and are simply not replicated.

**I6. Rendering.**
- **No renderer changes.** Bodies are loose chunks, and their headers and the loose BVH are
  rebuilt as they are now.
- (2026-10-14) The demos have their own renderer, `examples/17-physics-demos/demoRenderer/`:
  quarter-resolution G-buffer with a sun shadow ray, SSAO, and rotation-aware voxel
  reconstruction (cd5956b). Engine-side speedups it brought: `PJV_NO_ANIMATION` compiles the
  animated march out, and chunk headers carry a content box (6 texels) that rays skip on a miss.
- Render LOD (`renderLOD`) coarsens only the GPU copy. Collision always reads the full-resolution
  CPU geometry, so LOD can never change what collides.
- The animation envelope collides as its rest pose (`geometry`). Animated voxel parts that need to
  collide where they are drawn become kinematic parts later.
- A debug view of collision pieces needs a line or box drawing pass, which the renderer does not
  have yet. That is optional M3 work.

**I7. Events.** (As built 2026-10-15; the original list named ContactBegan/Ended, not built yet:
Roadmap R6.)
- Physics sends events through `Events`, delivered in the normal pump after PostUpdate:
  `BodyLeftWorld`, `BodyEmptied`, `JointBroke`, `PhysicsBodyRefused`, `SimulationSlow`.
  `BodyBroken` and `PiecesCleared` are delivered at once (`Events::trigger`), inside the step: the
  first before the pieces' bodies are made, so its handlers can shape them; the second before the
  entities it names go. (Was `BodySplit`, `BodyFractured` and `DebrisCleared`, 2026-10-15.)
- A queued event reaches a game after the step that caused it, so it cannot shape what that step
  made (a shard's layer or starting velocity) without lagging a step -- which also has to come out
  the same on every peer. Events whose handlers are meant to shape what was made move into the
  step: Roadmap R5.
- Contacts: `runtime::impacts(world)`, read by a FixedUpdate system after "physics" -- one per pair
  per step, the fastest, above minImpactSpeed.
- Jolt's contact callbacks run on its worker threads and never touch the registry. They only
  record into a buffer that is sorted by NetId after the step, so delivery order is deterministic.
- The sandbox's `Popped` → shockwave → removal chain works as now. The shockwave becomes impulse
  commands.

**I8. Picking and queries.**
- `utils::pickVoxel` stays the editor's exact-voxel tool.
- Gameplay uses `runtime::castRay` and friends. The sandbox's right-click pop moves to `castRay`.

**I9. Materials.**
- Superseded: built through the palette instead of an attachment. See "Physics materials" above.

**I10. Layering.** `utils/collision_geometry` includes neither Jolt nor EnTT, and Jolt headers are
reachable only from `src/runtime/physics/`. The `layering` ctest enforces both.

**I11. The editor.**
- It loads and saves the physics components like any other entity data.
- It does not simulate until play mode, which waits on its `ComponentRef` conversion (slot-reuse
  step 4).

## Milestones

Each milestone ends with a commit, its tests passing, the sandbox self-test passing and the
README tables updated.

**M0. Jolt builds.** Done 2026-10-09.
- As built: Jolt v5.6.0 at `external/JoltPhysics`; `runtime::PhysicsWorld` (Jolt behind a pimpl:
  primitive bodies, five layers, step, save/restore, state hash, stats) in
  `include/runtime/physics/physics_world.h`; 10 tests in `tests/unit/test_physics_world.cpp`.
- Physics sources build as the `projv_physics` object library, so Jolt's `-mavx2` and its defines
  reach only them (CMP0131 makes `$<LINK_ONLY>` work for the engine's link). `-ffp-contract=off`
  on our physics code too. Jolt is built with RTTI, because our code subclasses its interfaces.
- `restoreState` checks the body set before touching Jolt, and keeps a backup to undo a restore
  Jolt rejects partway: Jolt restores into whatever bodies exist and can fail midway.
- Found: a 5 cm sphere at 300 m/s (5 m a step) does not tunnel through a 10 cm wall even without
  continuous collision: Jolt's speculative contacts reach as far as a body moves in a step.
  Continuous collision stays the rule for small fast bodies (A8); it is the guarantee, speculative
  contacts a margin.

Planned:
- Submodule at a pinned tag; CMake options set (A7 build).
- Unit tests: a box falls onto a plane and comes to rest; the same run twice gives identical
  state hashes; `saveState` at tick 100, run to 200, restore, run to 200 again gives the same hash.
- The `layering` test is extended: Jolt is only reachable from `runtime/`.

**M1. Voxels to pieces to shapes.** Done 2026-10-09.
- As built: `utils::buildCollisionPieces` (`utils/collision_geometry.h`): the tree decoded into a
  dense box over the content bounds, merged greedily in z, y, x order. `GeometryBlob::contentStamp`
  (named a stamp, not a version: a process-wide counter, so a value names one content whichever
  pool slot holds it; a copy keeps it; `markBlobContentChanged` takes a fresh one).
  `PhysicsWorld::voxelShape` caches by (stamp, params key); one piece is a translated box, more a
  `StaticCompoundShape` with the piece index as sub-shape user data. Bodies scale it with a
  `ScaledShape`; their mass is volume x scale^3 x density with the shape's inertia; enhanced
  internal-edge removal is on for them. Unused shapes are pruned once a second of ticks.
- Tests: 6 for the pieces (exact cover, no overlap, deterministic, budget and size fallbacks,
  stamps), 5 for voxel bodies (rest height, a cup holds a ball, sharing and rebuild, pruning,
  determinism and restore).

Planned:
- `utils::buildCollisionPieces` (`Boxes`), blob `contentVersion`, and the shape cache.
- Tests (V1). No bodies yet.

**M2. Bodies.** Done 2026-10-09.
- As built: `Application::addSystem` takes a `SystemOrder` (before/after by name, honoured for
  systems added later). `runtime/physics.h`: `installPhysics`, `RigidBody` / `StaticCollider`
  (`physics.rigidbody` / `physics.static`), runtime-only `PhysicsBody`, commands (impulse,
  angular impulse, add/set velocity, teleport, gravity), `bodyPose`, `PhysicsBodyRefused` event.
- A body's shape is every chunk under its linked component (grid cells too), placed from the
  baked chunk headers relative to the component's world pose; primitives are fitted to the
  voxels' bounds in that frame. So a prefab (asset node at the centre, chunk offset by -half) and
  the arena grid take the same path. One compound per body around the shared unit shapes.
- Tick order inside "physics": doomed bodies, orphans, rebuilds (changed RigidBody, link made or
  lost), new bodies in entity order, commands in issue order, kinematic targets, step, readback.
  Entity order stands in for NetId order until M4.
- The sandbox: its integrator, sphere collisions and present system are gone; balls are spheres,
  crates and bombs their voxels; arena and pylons physics.static; shockwave = impulses, tractor =
  addVelocity, G = setGravity; a kill plane retires bodies thrown over the walls and the
  self-test fails if one falls from inside them. 3000-frame self-test: 740 spawns, 0 dropped
  contacts, 0 refused, 0 through the floor.
- Not done here (left for M5): moving a static collider, and grid edits rebuilding only the
  touched cells (today any change rebuilds the body via a RigidBody/StaticCollider patch).

Planned:
- System ordering (`before` / `after`, I1).
- `RigidBody`, `physics.static`, `PhysicsWorld` in FixedUpdate, the command queue, writeback and
  interpolation (A3), static grid bodies, and layers.
- The sandbox arena's entities.json gains `physics.static` (D3: nothing collides unasked).
- The sandbox's `physics()` replaced with this: balls as spheres, crates as voxels, bombs as
  voxels.
- The sandbox's own Body loses its integrator and keeps only game data (explosive).

**M3. Reliability harness.** Done 2026-10-09.
- As built: after every step, awake bodies are checked: non-finite ones go back to their last
  finite pose, at rest (`nonFiniteRecoveries`); ones outside `worldMin/worldMax` are reported
  once per excursion, and the runtime sends `BodyLeftWorld` and by default destroys the entity.
  Jolt clamps speeds (`maxLinearSpeed`, `maxAngularSpeed`). Mass floored at `minMass`, inertia at
  mass x `minGyrationRadius`^2 per principal axis. 80% of `maxBodies` warns once. Dropped fixed
  steps send `SimulationSlow` (warned at most every 300 ticks). `lastStepMilliseconds`.
- Jolt's thread pool is on (`threads = -1`: cores - 1, at most 7). A test holds results identical
  across 0, 4 and 7 threads.
- (2026-10-14, 1b5ee6e) Jolt's `JobSystemThreadPool` is replaced by our own `JobPool`
  (`src/runtime/physics/job_pool.h`): one queue, one mutex, a condition variable. Jolt's pool lost
  wake-ups on a hybrid CPU and stalled steps by 50-700 ms. Thread count still feeds nothing.
- **Found: tall stacks rock forever.** The 200-crate pile left 8-high columns swaying in a slow
  circle (0.2 m/s at the top, 1.1 s period) that never damped, so they never slept. Not convex
  radius, not edge removal, not inertia handling (all bit-identical), not Baumgarte or position
  steps. Restitution 0 and more velocity steps helped one layout and not others. Fixes, all three
  kept: restitution defaults to 0 (Jolt's default; bounce is asked for); 15 velocity steps; and
  **rest damping**: a dynamic body awake but within 5 cm and 0.1 rad of one pose for 1 s has its
  velocities x0.9 per step until it sleeps (in the snapshot, so restore stays exact). Rolling and
  spinning bodies are untouched (tested). Five pile layouts, with and without bounce, sleep
  within 8 s and stay put.
- Tests: threads, tunnel (1000 balls), tower, five piles, delete-during-contact, non-finite
  recovery, left-world (engine and runtime), mass floor, rest-damping guard rails; the soak
  (`ctest --preset soak`: 2000 bodies, 600 s, churn and explosions in a closed box) passes in
  26 s with no escapes, recoveries or dropped contacts.
- Not done: the mass-ratio warning (needs a contact listener, M6 brings one), a debug view of
  collision pieces, the edit-under-load test (M5: needs shape rebuild on edit).

Planned:
- The A8 guards, `PhysicsStats`, the V3 tests, and a sandbox stats line.
- The sandbox's measure mode reports physics time per tick next to frame and GPU time.

**M4. Network foundations.** Done 2026-10-09.
- As built: `PhysicsSettings::record` logs every simulation-changing call (body descs with their
  voxel pieces as data, destroys, impulses, velocities, poses, kinematic moves, gravity,
  restores) and each step's state hash; `PhysicsWorld::replay` reruns it in a fresh world and
  names the first step that differs. Logs save to and load from files (a bug-report format).
- `runtime/net.h`: `NetId` component (`projv.netid` in entities.json), an allocator that never
  reuses or collides with authored ids, `entityForNetId`.
- Bodies made in a step: NetId order, then the rest by entity. Commands: `PhysicsCommand` stamped
  (tick, issuer, sequence), targeted by NetId, `submitCommand`; local calls stamp the next tick,
  issuer 0. Due commands sorted by stamp; future ones wait; late ones apply next step and count.
- `physicsStateHash` (by NetId), `savePhysics` / `restorePhysics` (simulation, presentation
  poses, waiting commands; refuses a different body set without changing anything).
- Tests: exact replay of a busy scene (voxels, primitives, all commands, mid-run create/destroy,
  gravity change, a restore), tamper detection at the right step, truncation, file round trip,
  the runtime pipeline replaying exactly, two peers with reversed creation order agreeing every
  tick, stamp-not-arrival order, late and future commands, runtime rewind, NetId allocation.
- The sandbox self-test records and replays its whole session (3000 frames: 1065 steps, 243 KB,
  identical); spawned bodies get NetIds.

Planned:
- `NetId`, tick-stamped command log, and full save/restore including our side.
- The per-tick state hash and a record-and-replay test (V4).
- No transport.

**M5. Edits and destruction.** Done 2026-10-13.
- As built: each `PhysicsBody` keeps the (chunk, content stamp) list it was built from; a step
  checks bodies only when `latestBlobContentStamp()` moved, and rebuilds those whose list differs
  (NetId order). A dynamic body keeps its velocities across a rebuild. Destroying any body first
  wakes everything its bounds touch (`wakeBodiesAround`), so carving or deleting a floor drops
  what slept on it, and voxels added inside a body push it out.
- `utils/voxel_islands.h`: 6-connected islands in scan order; `splitVoxelIslands` keeps the
  largest, and makes each other island a duplicate of the component carved down to it (palette
  and materials kept), optionally as a root placed in the world. Occupancy decoding is shared
  with collision (`decodeOccupancy`).
- Runtime: a dynamic voxel body (a chunk, or a prefab: an asset with one chunk) whose voxels
  split becomes several bodies: new entities linked Root, the parent's RigidBody with an explicit
  mass shared by voxel count, NetIds if the parent had one, the parent's velocity at each piece's
  centre; `BodySplit`. Edited to nothing: `BodyEmptied` (not a refusal). `editVoxels` applies edits
  at the next step in issue order; `voxelComponentOf`, `voxelsInSphere`.
- Sandbox: bombs crater the arena (never its bottom layer) and bite crates, which split.
  3000-frame self-tests: 15-25 pieces blasted off, all simulated, books balanced, replays identical.
- Tests: islands, the split (materials, placement), carving under a sleeping crate, voxels added
  inside a resting one, deleting a floor, splitting with mass and motion, a prefab-shaped split,
  edit-command timing, a session with edits and splits replaying exactly, emptied bodies.
- Not done: static grids splitting (`physics.static` `splits`), grids or multi-chunk assets as
  dynamic bodies that split, and MutableCompoundShape for big grids (rebuilds are whole-body today;
  unchanged cells reuse their cached shapes, so only the compound is redone).

Planned:
- Shape rebuild at tick boundaries (A5).
- Island splitting.
- The sandbox's bombs carve voxels out of crates and the arena, and fragments fly.

**M6. Queries and character.** Done 2026-10-13.
- As built (queries): `PhysicsWorld::castRay` / `castSphere` / `overlapSphere` / `overlapBox` with a
  `QueryFilter` (layer mask, one body to skip). A hit carries the shape's normal and the path of
  compound indices to the sub-shape struck. Runtime `castRay` / `castSphere` / `overlap*` resolve that
  to entity, NetId and, for voxel bodies, chunk, voxel, component and material slot -- through each
  body's `PhysicsVoxelPart` list (recorded at build) and its *simulation* pose, not the drawn one.
- Tests: distances, normals, filters, sweeps, overlap order; and rays from all around random
  shapes -- one chunk, a two-chunk prefab offset in its asset, a grid -- name exactly the voxel,
  material slot and distance utils::pickVoxel does (normals match the face away from rounded edges).
- The sandbox pops with castRay.
- As built (character): `PhysicsWorld::createCharacter` etc. over Jolt's `CharacterVirtual`: a feet-
  origin capsule, an inner body (so rigid bodies collide with it), ground velocity + gravity +
  input + jump as in Jolt's sample, stair stepping and floor sticking, pushing at `maxStrength`.
  Characters update before the body step, in slot order; they are in the recording, the snapshot
  (version 3) and the state hash. Runtime: `CharacterController` (`physics.character`),
  `PhysicsCharacter`, `setCharacterInput` as a stamped command (`PhysicsCommand::CharacterInput`, by
  NetId from elsewhere), `characterState`; physics writes the Transform's position, interpolated.
- Tests: standing and walking speed, stairs and a wall, a voxel slope (a staircase: the step height
  limits it, not only the angle), jumping height and landing, pushing a crate, a ball not passing
  through, riding a kinematic platform, NetId input + rewind + replay, entities.json.
- Sandbox: V walks; the self-test walks a character through everything and checks it.
- Found and fixed on the way: a velocity carried over from a body at its speed cap (a rebuild, a
  cut piece) could come back a hair over and trip Jolt's creation assert -- `createBody` now caps
  velocities. And the sandbox's books could miss a piece retired before its BodySplit arrived; the
  books are now checked against actual Body construction and destruction counts.
- Release preset verified (Jolt asserts off, minimal logging): unit tests pass, and its 3000-frame
  self-test runs with no warnings or errors. It found one more edge: a body carved to nothing
  before its first step was refused with a warning instead of emptied; now an entity whose voxels
  changed after it asked for a body is `BodyEmptied` too (tested).

Planned:
- `castRay` / `castShape` / `overlap`, and a `CharacterVirtual` wrapper with a first-person
  controller in the sandbox.

**M7. Contours** (when the contour work is ready): the `ClippedVoxels` representation, the shared
CPU surface function, and the render-vs-collision normal test (A6.4).

**Separately:** editor play mode, after the editor's `ComponentRef` conversion (slot-reuse step 4).

## Verification

**V1. Geometry (unit, CPU).**
- For random blobs and every sculpt-test shape, the union of `Boxes` pieces equals the voxel set
  exactly, with no overlaps between pieces.
- The same blob decomposes to the same pieces in the same order (run twice, and on a copy).
- For every representation, every piece lies inside the voxel union (A6.1).
- The piece-budget fallback produces the content-bounds box.

**V2. Determinism (unit).**
- Run 1,000 ticks of a seeded scene twice: identical hash every tick.
- Save/restore at random ticks: identical continuation.
- Create the same bodies in a different ECS insertion order with the same NetIds: identical
  hashes (proves commands, not view order, decide).

**V3. Reliability (unit, headless, also the soak in CI).**
- **Tunnel:** 1,000 one-voxel balls at 200 m/s at a one-voxel-thick grid wall. Zero pass through.
- **Stack:** a 10-crate tower, and a 200-crate pile. Every body asleep within 5 s, no body
  drifting more than half a voxel after that.
- **Soak:** 2,000 bodies, 10 minutes of sim time with spawn, destroy and explode churn. Zero NaN
  events, zero escapes through the arena, constant memory, no contact drops.
- **Edit under load:** carve the floor out from under a pile; add voxels inside a resting body.
  Bodies fall or get pushed out, and none is left stuck or exploded.
- **Delete during contact:** destroy bodies mid-collision, every tick for 1,000 ticks. No dangling
  contacts, no crash.

**V4. Replay.**
- Record the sandbox self-test's command log; replay it headless; the final state hash matches.

**V5. Unchanged.**
- 78+ unit tests, sandbox self-test, 15-entities, editor self-tests.

## Open questions (none block M0 to M2)

- (Settled: threaded since M3, on our own `JobPool` since 2026-10-14.) Jolt's job system: start single-threaded for simplicity and determinism ease, and switch to
  `JobSystemThreadPool` in M3 once the determinism tests exist to prove it changes nothing.
  Jolt is deterministic with threads; the tests are what make that a fact here rather than a
  belief.

## What the demos showed (2026-10-13)

(History: what the demos found, with what has since been done. What is still open is carried into
"Roadmap" above, which is the order to work in.)

Findings from building the nine physics demos (a tenth, Slide, came with materials), in rough priority. **Physics materials come first**
(user's call): fracture strength, friction and restitution all hang off a voxel's material, so the
fracture work below is built on them, not before them.

1. **Materials** (built 2026-10-14, see above, strength and fracture pattern included): palette entry -> physics material (density, friction,
   restitution, strength, fracture pattern). Box merging stops at material
   boundaries; per-contact friction/restitution via a contact listener.
2. **Fracture and support.** (Support done 2026-10-15: `StaticCollider::splits`, below; dynamic
   grids still do not split.) Nothing checked structural support: a static wall cut at its base
   leaves the rest floating. After an edit, islands no longer connected to an anchor (ground, or
   voxels marked as anchors) should become dynamic. Grids / multi-chunk bodies can't split, so
   demo shards are capped at one chunk (16 voxels). (Done 2026-10-14: `runtime::fracture`, by the
   material's pattern -- see "Fracture"; demo 9 uses it, demo 2's blast still cuts slots itself.)
   Debris has no budget (cap, permanent sleep, merge back to static).
3. **Contact events.** (Impacts done 2026-10-14: `PhysicsWorld::impacts`, ordered, used for impact
   fracture; no per-entity event for games yet.) No "A hit B at P with impulse J" callback; demo 9
   predicted the strike geometrically three steps ahead. Needed for impact-driven fracture, sounds, damage and
   materials -- reported in deterministic order.
4. **Sleeping too eagerly.** Shards resting against the static wall freeze mid-fall until the
   boulder comes near again. Revisit sleep thresholds / rest damping for bodies in contact only
   with static geometry.
5. **Voxel collision fidelity.** Voxel gears jam on stair-steps at every handover (any resolution
   tried); only grid-aligned crosses work. Waits on contours; a ratio GearConstraint between two
   hinges is the reliable fallback.
6. **Joints.** No true lock (the trebuchet latch is a servo and sags 0.07 rad under 36 t); no
   mass-ratio warning; ropes/chains aren't a primitive (sling drawn by the demo, winch chain is 24
   bodies); Distance joints should probably collide by default.
7. **Kinematic.** ConstantVelocity added (kinematic bodies flying on their own; drawn from the
   simulation). Kinematic vs static don't collide (a launched ball passes through the floor):
   should be a per-body option.
8. **Scale.** The 2,700-crate pyramid scene drops steps while settling and takes ~14 s to sleep;
   tall stacks fell with 5 mm starting gaps. Want start-asleep for authored stacks, and a profile.
9. **Debug view** of shapes, contacts, joints and sleep state -- everything here was diagnosed
   from logs and framebuffer captures.
   (2026-10-14, 5be549a: the demos gained right-drag grab with a force-limited pull and click
   actions; a grab-style mouse spring may belong in the engine.)
10. (Engine, not physics) Prototypes are hidden by parking them 1 km down; missing that drew
    stray models at the origin. Want a not-drawn/template flag.

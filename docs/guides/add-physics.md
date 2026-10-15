# Add physics

Concepts:
[Physics](../concepts/physics.md). Working code: [`examples/16-sandbox`](../../examples/16-sandbox/)
(prefabs that bounce and explode), [`examples/17-physics-demos`](../../examples/17-physics-demos/)
(eleven set pieces: stacks, walls, joints, a car, a trebuchet, materials).

## 1. Install it

```cpp
#include "runtime/physics.h"

void startup(Application& app) {
    app.world.ctx().emplace<Scene>(utils::loadComposeFromDisk(folder));
    runtime::installSceneBridge(app);
    runtime::installPhysics(app);               // or (app, config): step 8
    runtime::loadPhysicsMaterials(app.world, "physics_materials.json");   // optional
    runtime::spawnEntities(app.world);
}
```

`installPhysics` registers `physics.rigidbody`, `physics.static`, `physics.character`,
`physics.joint` and `projv.netid`, and adds `"physics"` (FixedUpdate) and `"physics: present"`
(PostUpdate, before `"scene bridge"`).

## 2. Say what is solid

Nothing collides until an entity linked to the voxels asks:

```json
{ "name": "Floor", "link": 1, "components": { "physics.static": { "v": 1 } } },
{ "name": "Crate", "link": 2, "components": { "physics.rigidbody": { "v": 1, "motion": "dynamic", "density": 600 } } }
```

In code: `app.world.emplace<RigidBody>(entity, RigidBody{.density = 600})`. The body appears at the
next fixed step.

### `physics.rigidbody`

| Key | Default | Values |
|---|---|---|
| `motion` | `"dynamic"` | `"dynamic"`, `"kinematic"`, `"static"` |
| `shape` | `"voxels"` | `"voxels"`, `"sphere"`, `"box"`, `"capsule"` |
| `radius`, `halfExtents` `[x,y,z]`, `halfHeight` | 0 | Primitive sizes; 0 fits them to the voxels |
| `density` | 1000 | kg/m³ |
| `mass` | 0 | > 0 overrides density |
| `friction`, `restitution` | 0.6, 0 | |
| `linearDamping`, `angularDamping` | 0.05, 0.05 | |
| `gravityFactor` | 1 | |
| `layer` | `"moving"` | `"static"`, `"moving"`, `"debris"`, `"sensor"`, `"character"` |
| `quality` | `"auto"` | `"auto"` (continuous under 1 m), `"discrete"`, `"continuous"` |
| `material` | none | A physics material name: its density, friction and restitution win |
| `breaksOnImpact` | false | Hits may fracture it (step 6) |

An unknown word (`"motion": "floaty"`) refuses the whole value: kept in the file, no body.

### `physics.static`

| Key | Default | |
|---|---|---|
| `friction`, `restitution` | 0.6, 0 | |
| `layer` | `"static"` | |
| `splits` | false | Islands that lose contact with the component's y = 0 fall as bodies |
| `material` | none | Friction and restitution from the library |
| `breaksOnImpact` | false | |

### `physics.character`

| Key | Default | |
|---|---|---|
| `radius`, `height` | 0.3, 1.8 | Capsule; `Transform` position is the feet |
| `maxSlopeDegrees` | 50 | |
| `mass`, `strength` | 70, 300 | kg; N it pushes with |
| `stepHeight` | 0.4 | m |
| `layer` | `"character"` | |

```cpp
runtime::setCharacterInput(world, walker, walkVelocity, input.pressed(Key::Space), 6.0f);
runtime::characterState(world, walker).onGround;
```

### `physics.joint`

On its own entity, naming bodies by `NetId`:

```cpp
Joint hinge{.type = runtime::JointType::Hinge, .anchor = {0, 2, 0}, .axis = {0, 0, 1},
            .limitMin = -1.0f, .limitMax = 1.0f};
Entity j = runtime::connectBodies(world, door, frame, hinge);    // frame = NullEntity: the world
runtime::setJointMotor(world, j, true, 2.0f);                    // rad/s
```

| Key | Meaning |
|---|---|
| `type` | `"fixed"`, `"point"` (default), `"hinge"`, `"slider"`, `"distance"` |
| `a`, `b` | NetIds; `a` required, `b` 0 or absent = the world |
| `anchor`, `anchorB`, `axis` | World space when the joint is made |
| `limits` `[min, max]` | Angles (hinge), distances (slider, distance) |
| `spring` `[Hz, damping]` | Distance joints |
| `friction` | Hinge N·m, slider N |
| `motor` `{on, velocity, mode, target, limit, frequency}` | `mode`: `"velocity"` or `"position"` |
| `breakForce`, `breakTorque` | 0 = never breaks |
| `collideConnected` | false |

## 3. Push things

Commands apply at the next fixed step, in issue order:

```cpp
runtime::addImpulse(world, crate, {0, 40, 0});             // N·s
runtime::addVelocity(world, crate, {0, 5, 0});             // whatever the mass
runtime::setVelocity(world, crate, {0, 0, 0});
runtime::teleport(world, crate, {0, 10, 0}, core::quat(1, 0, 0, 0));
runtime::setGravity(world, {0, -20, 0});
```

**Don't patch a dynamic body's `Transform`**: it's ignored (logged once); teleport. Kinematic bodies
are the reverse: patch the `Transform`, or add a `ConstantVelocity`.

## 4. Ask where things are

```cpp
auto hit = runtime::castRay(world, camera.position, camera.forward(), 80.0f, 0xffffffffu, camera.walker);
if (hit && hit->onVoxel)
    core::info("hit voxel {} {} {} of component {}", hit->voxel.x, hit->voxel.y, hit->voxel.z, hit->component);

for (Entity e : runtime::overlapSphere(world, blast, 5.0f)) runtime::addImpulse(world, e, push(e));

runtime::BodyState s = runtime::bodyPose(world, crate);   // simulated pose
```

The `Transform` is interpolated by `Time::fixedAlpha`; gameplay reads `bodyPose`.

## 5. Give voxels materials

`physics_materials.json`, loaded with `runtime::loadPhysicsMaterials(world, path)`:

```json
{ "v": 1,
  "materials": [
    { "name": "ice",    "density": 917,  "friction": 0.01 },
    { "name": "rubber", "density": 1100, "friction": 1.0, "restitution": 0.9 },
    { "name": "ashlar", "density": 800,  "friction": 0.8, "strength": 1000, "fracture": "shatter", "shardSize": 0.4 }
  ] }
```

Name them from palette entries (`"physics": "ice"`, `utils::setMaterialPhysics`) or a body
(`"material": "rubber"`). Format:
[physics_materials.json](../reference/formats/physics-materials-json.md).

## 6. Break things

Only voxels whose material has a `strength` break.

```cpp
runtime::fracture(world, hit->entity, hit->position, 4000.0f /* J */, camera.forward() * 3.0f);
```

- **On impact**: `"breaksOnImpact": true`.
- **Carving** (a crater):
  ```cpp
  auto crater = runtime::voxelsInSphere(world, ground, centre, 3.0f);
  runtime::editVoxels(world, ground, crater);
  ```
- **Your own pattern**: `runtime::registerFracturePattern(world, "slabs", fn)`, then
  `"fracture": "slabs"`. Must be deterministic. See
  [`runtime/fracture.h`](../../include/runtime/fracture.h).
- **Debris** is capped at `PhysicsConfig::maxDebris` (3000), oldest first.

## 7. React to what happened

```cpp
app.events().on<BodySplit>([&](const BodySplit& e) {
    for (Entity piece : e.pieces) app.world.emplace<MyGameTag>(piece);
});
app.events().on<BodyFractured>([&](const BodyFractured& e) { playSound(e.at, e.energy); });
app.events().on<BodyEmptied>([&](const BodyEmptied& e)   { app.world.destroy(e.entity); });
app.events().on<BodyLeftWorld>([&](const BodyLeftWorld&) { /* entity is already gone */ });
app.events().on<JointBroke>([&](const JointBroke& e)     { /* e.force, e.torque */ });
```

Full list: [Physics → Events](../concepts/physics.md#events).

## 8. Tune it

```cpp
runtime::PhysicsConfig config;
config.settings.gravity = {0, -20, 0};
config.settings.worldMin = core::vec3(-500);  config.settings.worldMax = core::vec3(500);
config.settings.record = true;                    // for PhysicsWorld::replay
config.maxDebris = 1000;
config.destroyBodiesThatLeaveTheWorld = true;
runtime::installPhysics(app, config);
```

Settings: [Physics → Guards](../concepts/physics.md#guards-input-is-checked-never-asserted-on).

## 9. Check it

```bash
DEMOS_SELFTEST=1 ./physics_demos      # in build/examples/physics_demos
ctest --preset dev
ctest --preset soak                   # after physics changes
```

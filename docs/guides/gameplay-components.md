# Gameplay components

Concepts: [Entities](../concepts/entities.md). Working code:
[`examples/15-entities`](../../examples/15-entities/) (spin and orbit),
[`examples/16-sandbox`](../../examples/16-sandbox/) (spawners, prefabs).

## 1. Define the component

The same type is in the registry and in the file.

```cpp
struct Spin { float speed = 1.0f; };                                    // radians per second
struct Orbit { float speed = 1.0f; float radius = 0.0f; float angle = 0.0f; };
```

## 2. Say how it reads and writes JSON

```cpp
template<> struct projv::runtime::ComponentTraits<Spin> {
    static constexpr const char* key = "example.spin";      // your own prefix; "projv." is reserved
    static constexpr uint32_t version = 1;                  // written as "v"
    static nlohmann::json save(const Spin& s) { return {{"speed", s.speed}}; }
    static std::optional<Spin> load(const nlohmann::json& j, uint32_t fileVersion) {
        return Spin{j.value("speed", 1.0f)};
    }
};

// Runtime state (angle) is not saved.
template<> struct projv::runtime::ComponentTraits<Orbit> {
    static constexpr const char* key = "example.orbit";
    static constexpr uint32_t version = 1;
    static nlohmann::json save(const Orbit& o) { return {{"speed", o.speed}, {"radius", o.radius}}; }
    static std::optional<Orbit> load(const nlohmann::json& j, uint32_t) {
        return Orbit{j.value("speed", 1.0f), j.value("radius", 0.0f), 0.0f};
    }
};
```

- `save` returns an object; `"v"` is added for you.
- `load` sees `"v"` too; `fileVersion` is 1 when absent.
- Refusing (`std::nullopt` or a nlohmann exception) keeps the value as written, saved back, with a warning.

## 3. Write `entities.json`

Beside `compose.json`; `link` is a `compose.json` entry's `"id"`:

```json
{
  "version": 1,
  "entities": [
    { "name": "Sun",    "link": 1, "components": { "example.spin":  { "v": 1, "speed": 0.4 } } },
    { "name": "Planet", "link": 2, "components": { "example.orbit": { "v": 1, "speed": 0.35, "radius": 45 } } }
  ]
}
```

Also `"link": "document"` (the folder's own node when nested or a prefab), no link (standalone), and
`"onUnlink": "destroy"`. Format:
[entities.json](../reference/formats/entities-json.md).

## 4. Register, then spawn

```cpp
void startup(Application& app) {
    auto& scene = app.world.ctx().emplace<Scene>(utils::loadComposeFromDisk(folder));
    runtime::installSceneBridge(app);
    runtime::registerComponent<Spin>(app.world);              // before spawning
    runtime::registerComponent<Orbit>(app.world);
    app.world.on_construct<Orbit>().connect<&startOrbit>();   // per-entity setup, see step 6
    runtime::spawnEntities(app.world);
}
```

Inner folders spawn first; an outer file naming the same component adds to, and overrides, the
inner entity.

## 5. Drive it with a system

```cpp
void spinSystem(Application& app) {
    const float dt = app.time().delta;
    for (auto [entity, spin] : app.world.view<Spin>().each()) {
        // patch, not get: the Scene bridge only writes Transforms it was told changed.
        app.world.patch<Transform>(entity, [&](Transform& t) {
            t.rotation = glm::normalize(glm::angleAxis(spin.speed * dt, core::vec3(0, 1, 0)) * t.rotation);
        });
    }
}

app.addSystem(Stage::Update, "spin", spinSystem);
```

The bridge moves the voxels in PostUpdate; `flushSceneUpdates` once per frame in Render.

## 6. Setup that needs the entity

`on_construct` runs after the link seeded the `Transform`:

```cpp
void startOrbit(World& world, Entity entity) {
    if (const auto* t = world.try_get<Transform>(entity))
        world.get<Orbit>(entity).angle = std::atan2(t->position.z, t->position.x);
}
```

## 7. Make entities in code and save them

```cpp
Entity sun = runtime::spawnComponent(app.world, sunHandle, LinkMode::Root);
app.world.emplace<runtime::Authored>(sun, runtime::Authored{INVALID_COMPONENT_HANDLE, "Sun"});
app.world.emplace<Spin>(sun, 0.4f);

runtime::saveEntities(app.world, INVALID_COMPONENT_HANDLE, folder);       // the top-level file
runtime::saveEntities(app.world, planetHandle, folder + "/Planet");       // a nested folder's file
```

- **Each entity belongs to one file**: `Authored::document`'s, else the file its linked component
  is an entry of.
- **A linked entity's `Transform` is not written**; it lives in `compose.json` (`saveComposeToDisk`).
- **Unregistered components are written back unchanged.**

## 8. Prefabs

A prefab folder's `"document"` entity carries its components:

```json
{ "version": 1,
  "entities": [ { "name": "ball", "link": "document",
                  "components": { "physics.rigidbody": { "v": 1, "shape": "sphere", "mass": 1.0 },
                                  "sandbox.body":      { "v": 2 } } } ] }
```

```cpp
Entity ball = runtime::instantiatePrefab(app.world, "prefabs/ball", {0, 10, 0});
app.world.destroy(ball);      // OnUnlink::Destroy (the default here): the voxels go too
```

Read once and cached; instances share geometry until edited. `runtime::clearPrefabCache(world)`
drops the cache.

## Events

```cpp
struct Popped { Entity entity; };
app.events().on<Popped>([&](const Popped& e) { app.world.destroy(e.entity); });
app.events().send(Popped{entity});            // delivered after PostUpdate
```

See [The frame loop](../concepts/frame-loop.md#events).

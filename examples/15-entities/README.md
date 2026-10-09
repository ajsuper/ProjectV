# 15 — Entities

Hello Voxel with things that move. The runtime on top of the voxels: the Application's stages,
`Time`, `Input`, events, the **Scene bridge** that links entities to voxel components, and
**authored entities** saved beside the voxels.

```bash
cd build/examples/entities && ./entities
```

A sun turns in place. A planet orbits it, and a moon orbits the planet. W/S/A/D move, R/F go up and
down, the mouse looks (Esc releases the cursor, a click recaptures it), and Space pauses time. Close
the window to quit.

## Voxels in one file, entities in the other

The scene is a folder, `scene/`, staged beside the binary. `compose.json` holds the voxels, and gives
each entry an `"id"`. `entities.json` beside it holds the entities:

```json
{ "version": 1,
  "entities": [
    { "name": "Sun",    "link": 1, "components": { "example.spin":  { "speed": 0.4, "v": 1 } } },
    { "name": "Planet", "link": 2, "components": { "example.orbit": { "radius": 45.0, "speed": 0.35, "v": 1 } } }
  ] }
```

`"link"` names the component this entity drives, by its id in the same folder. `"components"` are
the ECS components themselves: `Spin` and `Orbit` are what the file says *and* what runs. There is no
second "authored" type and no translation step. This program defines each component once, with
`ComponentTraits` saying how it reads and writes JSON, and registers it:

```cpp
projv::runtime::registerComponent<Spin>(app.world);
projv::runtime::registerComponent<Orbit>(app.world);
app.world.on_construct<Orbit>().connect<&startOrbit>();   // setup that needs the Transform
projv::runtime::spawnEntities(app.world);                 // every entities.json in the scene
```

`startOrbit` is the one thing a file cannot say: where the orbit starts. It runs when the component
is made, after the link has given the entity its Transform, so it starts from where the asset put
it.

## Two kinds of link

The sun and the planet are root components, so their entities get **Root** links: the entity's
Transform *is* the component's transform, in the world.

The moon is not a root. It is a component *inside* the planet, so it moves with the planet for free:
that's the Scene hierarchy doing what it already did. Its entity is declared in the *planet's* own
file, `scene/Planet/entities.json`, because it is about something inside the planet. Linking a
component that has a parent makes a **Part** link, whose Transform is measured in the parent's space.
So the moon's orbit is a circle around the planet, wherever the planet is.

The two hierarchies never fight. A Root link only ever names a component with no Scene parent, and a
Part link writes the component's local transform, which is what the Scene hierarchy composes.

## Why each body is an asset

Every body is an Asset whose origin is its centre, with the voxel ball inside it, offset by its
radius. A chunk's own origin is its *corner*, so spinning the ball directly would swing it around its
corner. The balls are also in alternating wedges of two colours: a single-colour sphere turning about
its own axis looks exactly like one standing still.

## Regenerating the scene

```bash
./entities --write-scene ../../../examples/15-entities/scene
```

builds the bodies through the edit queue and saves the voxels with `saveComposeToDisk`. It then
loads the folder back, links entities, adds `Spin` and `Orbit`, and writes each folder's
`entities.json` with `saveEntities`, the same path a game's save would take.

## Engine features used

- `projv::Application`, `Stage`, `Time` (`core/application.h`, `core/time.h`)
- `graphics::installPlatform`, `graphics::setCursorCaptured`, `projv::Input` (`graphics/input.h`)
- `runtime::installSceneBridge`, `spawnComponent`, `Transform`, `LinkMode` (`runtime/scene_bridge.h`)
- `runtime::ComponentTraits`, `registerComponent`, `spawnEntities`, `saveEntities`, `Authored`
  (`runtime/entities.h`)
- `utils::loadComposeFromDisk`, `saveComposeToDisk`, `graphics::flushSceneUpdates`

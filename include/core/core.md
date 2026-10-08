## Distinction:

Core handles the most fundamental logic for the engine: the application, its loop and its clock. It
has no dependency on any voxel or graphics functionality, and could be used in a project that has
neither. Its only third-party dependencies are [EnTT](https://github.com/skypjack/entt), for the
entity registry, and glm.

### Examples:
```markdown
Note: Below are mock functions; they don't exist in the engine.
```
```cpp
// ✅ This would belong in core, as it applies to many uses other than voxels or graphics.
app.addSystem(projv::Stage::Update, "ai", thinkAboutThings);

// ❌ This would not belong in core, as it is most likely not useful outside a graphical application.
projv::core::initializeWindow();
```

### Core modules:
- **application** (`core/application.h`) -> `projv::Application`: the world, the stages and the frame
  loop. Systems are added to stages with `addSystem` and run in the order they were added. The
  stages are Startup, then each frame PreUpdate, FixedUpdate (zero or more times), Update,
  PostUpdate, the event pump and Render, then Shutdown. `CloseRequested` and `WindowResized` are
  declared here; the platform layer (`graphics/input.h`) sends them.
- **world** (`core/world.h`) -> `projv::World` (an `entt::registry`) and `projv::Entity`. The names
  engine code uses for EnTT's types. It is not a wrapper: use views, signals and `ctx()` as EnTT
  documents them. Global resources live in `world.ctx()`.
- **time** (`core/time.h`) -> `projv::Time`: frame delta, the fixed-step accumulator, time scale and
  the spiral-of-death guard. An engine resource: `app.time()`.
- **events** (`core/events.h`) -> `projv::Events`: typed queues delivered once per frame. Anything
  sent during a pump waits for the next one. An engine resource: `app.events()`.
- **math**, **log**, **paths** -> glm aliases, the logging categories, `executableDirectory()`.

The layer above core that joins entities to voxel components is `runtime/` (see
`runtime/scene_bridge.h`). Core knows nothing about it.

### More

For more information on this project, visit our [README.md](/README.md)

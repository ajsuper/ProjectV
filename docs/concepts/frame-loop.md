# The frame loop

`projv::Application` owns the world, the stages and the loop. A program is systems added to stages.
Headers: [`core/application.h`](../../include/core/application.h),
[`core/time.h`](../../include/core/time.h), [`core/events.h`](../../include/core/events.h).

## Stages

```text
run()
 ├─ Startup ........................................... once
 ├─ loop until closeRequested:
 │   ├─ Time::advance(clock())     → how many fixed steps this frame owes
 │   ├─ PreUpdate                  platform: poll window, fill Input
 │   ├─ FixedUpdate × N            physics, anything frame-rate independent (fixedDelta each)
 │   ├─ Update                     gameplay (Time::delta)
 │   ├─ PostUpdate                 present physics poses, Scene bridge syncs Transforms
 │   ├─ Events::pump()             every event sent this frame is delivered here
 │   └─ Render                     draw
 └─ Shutdown .......................................... once
```

## Systems

A system is a function of the application, never a lambda that captures it:

```cpp
using System = std::function<void(Application&)>;

app.addSystem(Stage::Update, "spin", [](Application& app) {
    float dt = app.time().delta;
    for (auto [e, spin] : app.world.view<Spin>().each())
        app.world.patch<Transform>(e, [&](Transform& t) {
            t.rotation = glm::angleAxis(spin.speed * dt, core::vec3(0, 1, 0)) * t.rotation;
        });
});
```

- **Within a stage, systems run in the order added**, unless a `SystemOrder` says otherwise:
  `addSystem(stage, name, fn, {.before = "scene bridge"})`. A constraint naming a system not added
  yet still holds when it is added; unsatisfiable ones are logged and the system goes last.
- `app.systemNames(stage)` lists a stage's systems.

## Resources: `world.ctx()`

```cpp
auto& scene = app.world.ctx().emplace<Scene>(utils::loadComposeFromDisk(path));   // once
auto& scene = app.world.ctx().get<Scene>();                                        // after
auto* maybe = app.world.ctx().find<Scene>();                                       // nullptr if absent
```

The engine's own resources are there too: `Time` (`app.time()`), `Events` (`app.events()`), `Input`
(after `installPlatform`).

## Time

```text
real frame time ──clamp(maxDelta)──▶ unscaledDelta ──× scale──▶ delta            (Update)
                                                   └──▶ accumulator ──▶ N × fixedDelta (FixedUpdate)
                                                                 └──▶ fixedAlpha ∈ [0,1)  (interpolation)
```

| Field | Meaning |
|---|---|
| `delta` | This frame, scaled and clamped |
| `unscaledDelta` | Clamped, not scaled |
| `fixedDelta` | Default 1/60 s |
| `fixedAlpha` | Progress into the next fixed step, for interpolation |
| `scale` | 0 pauses `delta` and FixedUpdate. |
| `maxDelta` | Frame-time clamp, default 0.25 s |
| `maxFixedStepsPerFrame` | Default 8; steps beyond it are dropped and counted in `droppedFixedSteps` |

Tests replace `app.clock` and drive `runStartup()` / `runFrame()` / `runShutdown()` directly.

## Events

Typed queues, delivered once per frame at one point: after PostUpdate.

```cpp
struct DoorOpened { Entity door; };

app.events().on<DoorOpened>([](const DoorOpened& e) { /* ... */ });   // connect
app.events().send(DoorOpened{door});                                     // delivered at the next pump
```

- **`send` queues**; handlers never run mid-system.
- **An event sent during a pump waits for the next one**, so an event cycle is one event per frame,
  not a hang.
- **`trigger` delivers immediately.** Rare; say why at each use.
- Handlers run in connection order. `disconnect(id)` is safe from inside a handler.
- Main thread only.

Events the engine sends:

| Event | Sent by | When |
|---|---|---|
| `CloseRequested` | platform | Window close. Ends the app while `app.closeOnRequest` (default true) |
| `WindowResized` | platform | Framebuffer size changed |
| `EntityLinked`, `ComponentDestroyed`, `ComponentTransformChanged` | Scene bridge | See [Entities](entities.md) |
| `BodyBroken`, `PiecesCleared`, `ContactBegan`, `BodyLeftWorld`, `JointBroke`, ... | physics | See [Physics](physics.md#events) |

## Closing

```cpp
app.closeRequested = true;          // ends the loop after this frame; Shutdown then runs
app.closeOnRequest = false;         // handle CloseRequested yourself
```

`Application` is neither copyable nor movable.

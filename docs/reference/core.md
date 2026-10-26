# core

The application, loop, clock, events, maths, logging and paths. No voxel or GPU dependency.
Third-party: EnTT, glm. Namespaces: `projv` (application, world, time, events), `projv::core`
(maths, log, paths).

| Belongs in core? | |
|---|---|
| ✅ `app.addSystem(Stage::Update, "ai", think)` | Any program needs it |
| ❌ `core::loadComposeFromDisk()` | Belongs in `utils/` |

Explained in: [The frame loop](../concepts/frame-loop.md), [Logging](../guides/logging.md).

## `core/application.h`

[`include/core/application.h`](../../include/core/application.h): owns the world, the stages and
the loop.

| Name | What |
|---|---|
| `Stage` | `Startup`, `PreUpdate`, `FixedUpdate`, `Update`, `PostUpdate`, `Render`, `Shutdown` |
| `System` | `std::function<void(Application&)>` |
| `SystemOrder` | `{before, after}`: order a system relative to others by name |
| `Application` | `world`, `addSystem`, `run`, `runFrame`/`runStartup`/`runShutdown`, `time()`, `events()`, `clock`, `closeRequested`, `closeOnRequest`, `frameCount`, `systemNames` |
| `CloseRequested`, `WindowResized` | Events the platform sends |
| `stageName(Stage)` | The stage's name, for logs |

## `core/world.h`

[`include/core/world.h`](../../include/core/world.h): the names engine code uses for EnTT's types.

| Name | What |
|---|---|
| `World` | `entt::registry`. Resources live in `world.ctx()`. |
| `Entity` | `entt::entity` |
| `NullEntity` | `entt::null` |

## `core/time.h`

[`include/core/time.h`](../../include/core/time.h): the frame clock and the fixed-step clock.

| Name | What |
|---|---|
| `Time` | Settings: `scale`, `fixedDelta`, `maxDelta`, `maxFixedStepsPerFrame`. State: `delta`, `unscaledDelta`, `elapsed`, `fixedAlpha`, `frame`, `fixedStep`, `droppedFixedSteps`. |
| `Time::advance(now)` | Moves the clock, returns the fixed steps owed |

## `core/events.h`

[`include/core/events.h`](../../include/core/events.h): typed event queues pumped once per frame.

| Name | What |
|---|---|
| `Events::on<T>(fn)` | Connect a handler; returns a `Connection` |
| `Events::send(event)` | Queue for the next pump |
| `Events::trigger(event)` | Deliver now (rare; say why) |
| `Events::disconnect(id)` | Remove a handler, safe from inside one |
| `Events::pump()` | Deliver what was queued before the pump began |
| `Events::pendingCount<T>()` | Queued events of a type |

## `core/log.h`

[`include/core/log.h`](../../include/core/log.h): logging categories over spdlog, compiled out
when disabled.

| Name | Tag | Switch |
|---|---|---|
| `core::trace` | `[TRC]` | `PROJV_LOG_TRACE` |
| `core::perf` | `[PRF]` | `PROJV_LOG_PERF` |
| `core::edit` | `[EDT]` | `PROJV_LOG_EDIT` |
| `core::render` | `[RND]` | `PROJV_LOG_RENDER` |
| `core::info` / `warn` / `error` | `[INF]` `[WRN]` `[ERR]` | `PROJV_LOG_INFO` / `WARN` / `ERROR` |
| `core_<category>_every(N, fmt, ...)` | | Log every Nth call |
| `core_<category>_every_ms(MS, fmt, ...)` | | Log at most once per MS milliseconds |

## `core/math.h`

[`include/core/math.h`](../../include/core/math.h): glm aliases.

| Name | What |
|---|---|
| `vec2..4`, `ivec2..4`, `dvec2..4`, `mat2..4`, `quat` | glm types |
| `normalize`, `dot`, `cross`, `mix`, `clamp`, `lookAt`, ... | glm functions, re-exported |
| `projectionMatrix(fovy, aspect, near, far, homogeneousDepth)` | Via `bx::mtxProj` |
| `evaluateCurve(input, points)` | A piecewise spline |

## `core/paths.h`

[`include/core/paths.h`](../../include/core/paths.h)

| Name | What |
|---|---|
| `executableDirectory()` | The running binary's directory, symlinks resolved |

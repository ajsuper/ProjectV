# 18 — World loading

A terrain 768 m across, loaded around the camera as you fly. Only nearby blocks are in memory; the
rest stay on disk until you come near.

```bash
cd build/examples/world_loading && ./world_loading
```

The first run writes the terrain to `terrain/` beside the binary (`--regenerate` writes it again).
W/S/A/D move, R/F up/down, Shift is fast, the mouse looks. Stats are logged once a second.

| Key | Rule |
|---|---|
| 1 | `loadNear(96, 192)` (the default) |
| 2 | `loadNear(32, 64)` |
| 3 | `loadEverything()` |

## One rule, the engine does the rest

```cpp
projv::runtime::WorldLoadingConfig config;
config.policy = projv::utils::loadNear(96.0f, 192.0f);   // or loadInside(box), loadEverything(), a lambda
projv::runtime::openWorld(app, "terrain", config);        // the tree, no voxels

// each frame:
projv::runtime::setViewers(app.world, {{camera.position}});
// ... "world loading" (PreUpdate) asks the rule, reads blocks on a worker, makes and frees chunks
projv::graphics::flushSceneUpdates(scene, gpuData);       // as always: new chunks are uploaded
```

A rule is any function from a block (its bounds, component, cell, what it said last time) and the
viewers to `Absent`, `Kept` or `Active`:

```cpp
projv::runtime::setLoadRule(app.world, {[](const projv::utils::LoadCandidate& block, auto viewers) {
    return block.bounds.min.y < 64.0f ? projv::utils::Presence::Active : projv::utils::Presence::Absent;
}});
```

A player entity can be the viewer instead: `world.emplace<projv::LoadViewer>(player)`. Entities
linked to a component come and go with its blocks (`runtime/loading.h`).

## Checks

| Command | What |
|---|---|
| `./world_loading --headless-check` | no window: flies the course with the loader alone; peak memory and every block within 64 m loaded |
| `WORLD_LOADING_SELFTEST=1 ./world_loading` | the same course, drawn; prints `WORLDTEST` lines and exits 0 or 1 |

Design: `docs/plans/world-loading.md`.

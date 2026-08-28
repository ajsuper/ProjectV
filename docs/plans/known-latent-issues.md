# Known latent issues

Real defects that are not currently triggered, found while packaging the engine
(see the tooling/packaging plan). Each is recorded here rather than fixed in place
because fixing it is a change of its own scope.

---

## 1. `assignSystemStage` captures a dangling reference

`src/core/ecs.cpp`, `assignSystemStage`:

```cpp
app.Startup = [=, &app]() { system(app); };
```

The lambda captures `app` **by reference** and is stored *inside* `app`. That is fine
for the way every current example uses it — `createApp()` into a local, register stages,
`runApplication(app)`, all on one stack frame — but it means an `Application` cannot be
moved, copied, returned from a factory, or stored in a container. Any of those leaves all
four stage callbacks pointing at the old object.

Not triggered today because nothing does that.

**Why it is not a one-line fix.** The stage signature is `void(Application&)` while the
stored type is `std::function<void()>`, so the `Application` has to come from *somewhere*
at call time. Fixing it properly means storing `std::function<void(Application&)>` and
having `runApplication` pass itself in — which changes the type of the four members and
touches every stage registration. Worth doing, but deliberately.

**Until then:** create an `Application` where it will live and do not move it.

---

## 2. `core::perf` is unqualified in two examples' profiler blocks

`docs/examples/PathTracer/main.cpp:269` and `docs/examples/edit_demo/main.cpp:467` call
`core::perf(...)` where the enclosing scope has no `core` — it needs `projv::core::perf`,
which is what `ScenePreviewer` and `terrain_generator` correctly use.

Both sites sat inside `#if defined(PROJV_ENABLE_PERF)`, and neither example's Makefile defined
that macro, so the block was never compiled and the error never surfaced.

**Resolved.** It surfaced on the first build after the examples moved to the unified CMake
build, which puts the log definitions on the target with `PROJV_LOG_PERF` on by default —
exactly as predicted here. Both are now `projv::core::perf`.

---

## 3. Public headers leak implementation dependencies

`<iostream>` is included by eight public headers (`gpu_interface.h`, `manage_resources.h`,
`perform_renderer.h`, `render_instance.h`, `type_mapping.h`, `voxel_math.h`,
`voxel_management.h`, `ecs.h`), `disk_io.h` pulls in `nlohmann/json.hpp`, and
`render_instance.h` pulls in `GLFW/glfw3.h` and `glfw3native.h`.

The consequence is that any consumer of the graphics API needs spdlog, glm, bx, bgfx,
GLFW and nlohmann on its include path — which is exactly what the example Makefiles were
forced to encode by hand.

Deferred deliberately: removing them means moving definitions into the `.cpp` files and
touching many call sites.

---

## 4. `core/math.h` forced bx's debug configuration on every consumer

`include/core/math.h` opened with an unconditional `#define BX_CONFIG_DEBUG 1` before including
`bx/math.h`. bx does require the macro to be set before its headers, but defining it
unconditionally in a public header meant every translation unit that included `core/math.h` --
which is most of the engine and every example -- compiled bx with assertions and debug
configuration on, **including release builds**.

It went unnoticed because the old build never set the macro itself, so there was nothing to
collide with. bgfx.cmake sets it per configuration, which turned it into a visible
`BX_CONFIG_DEBUG redefined` warning on every file.

**Resolved.** The definition is now guarded by `#ifndef` and falls back to deriving from
`NDEBUG`. Note this is a **behaviour change** for release builds: they now get bx's release
configuration, which is what they should always have had.

---

## 5. Most bundled scenes are `.data` version 1 and load as zero chunks

The loader accepts container versions 2 and 3 and rejects version 1 with a clear message, then
skips the block. A scene whose every `.data` is v1 therefore *parses* — its `compose.json` is
fine, its components are counted — and contains no geometry at all.

Fourteen bundled `.data` files are still v1, about 250 MB:

| Asset | Consequence |
|---|---|
| `PathTracer/SponzaScene/model.data` | **The path tracer renders an empty scene.** All six renderers start, submit frames and show nothing. AdvancedRenderer, which loads the same scene, is empty for the same reason. |
| `PathTracer/QuadSponza/sponza_{a,b,c,d}/model.data` | Same, for the four-instance scene. |
| `edit_demo/SponzaScene/model.data` | Same. |
| `MeshVoxelizer/trees/*` (8 assets) | Every bundled tree asset. |
| `ScenePreviewer/scenes/{Sibenik,LostEmpire}/model.data` | Already documented in that example's README. |
| `ScenePreviewer/scenes/StonehillCastle/model.data` | Unreferenced leftover; that scene loads from its v2 components. |

This is pre-existing content rot — the files have not changed since 2026-08-01 — and it is
invisible from the outside because nothing fails loudly. An example that starts, renders and
exits cleanly looks fine; only the chunk count in the log says otherwise.

Fixing it means re-voxelizing from the source meshes, which are not in the repository. Until
then, "the example runs" is not evidence the example works: check the `Loaded N chunk(s)` line.

## 6. `editing_p1` fails 6 of its 86 self-checks

Against a scene that loads (`ScenePreviewer/scenes/SmallVox`), the driver reports 80 `[OK]` and
6 `CHECK FAILED`:

```
Sponza grid dims unchanged after in-bounds edit
programmatic: seed has 5 voxels
programmatic: voxel count grew by 3
p3-convert: cell 0 has 6 voxels (5 seed + 1 add)
grid-test: chunk 0 has 5 voxels (3 seed + 2 adds)
grid-test: chunk 1 has 3 voxels (2 seed + 1 add)
```

The first is expected — it names Sponza, and Sponza cannot be loaded (issue 5 above), so the
substitute scene has different dimensions. The other five build **synthetic** scenes in the
driver itself and are independent of which scene was loaded: they are voxel counts after an
edit, in the editing subsystem.

**Not caused by the build work.** Verified by stashing every `include/` and `src/` change from
this effort, rebuilding, and re-running: identical results, the same 80 pass and the same 6 fail.

Plausibly these have been failing for some time without being seen: the driver's default scene is
one of the v1 assets, so it could not run at all without being handed a different scene by hand.
Worth a real look when the editing phases resume.

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

---

## 7. A relative symlink across the example tree

`tests/manual/edit_demo/include/stb_image.h` was a symlink to
`../../PathTracer/include/stb_image.h` -- a relative path into a *different* example's vendored
copy. It broke the moment either directory was renamed, and surfaced as
`fatal error: stb_image.h: No such file or directory` rather than as anything mentioning a symlink.

**Resolved.** Vendored properly, which is what the other two consumers of that header already do.

Worth noting as a pattern to avoid: sharing a vendored header between examples by symlink couples
their directory layouts together invisibly, and a broken symlink reports as a missing file.

## 8. `edit_demo` baked its author's source tree into the binary

It called `fs::current_path(fs::path(__FILE__).parent_path() / "../PathTracer")` -- resolving
`__FILE__` at runtime, so the binary only worked on a machine that still had the source checkout
at the path it was compiled on, with that directory name.

**Resolved.** It now derives its own location with `projv::core::executableDirectory()` and enters
the renderer gallery's staged directory, because the renderer it borrows names its shaders relative
to the working directory.

---

## 9. `setUniformToValue` rejects const arguments, at runtime

`setUniformToValue` is a template that dispatches on the deduced type. Handed a `const vec3` --
which is what you get from an ordinary `const` local, not just from a const struct member -- the
dispatch finds no match and logs:

```
Function: setUniformToValue. Typename T for data is unknown.
Missing uniform value: 'cameraDir'.
```

The uniform then silently never reaches the shader. It is a **runtime** failure for what is a
type-level mistake, and the diagnostic, while unusually helpful, only fires once the program is
running.

Hit while writing `00-hello-voxel`, whose uniform locals were declared `const` out of habit.

**Worth fixing properly** by decaying the deduced type (`std::remove_cv_t<std::remove_reference_t<T>>`)
before the dispatch, so const and reference arguments simply work. That is a small change to a
template in `gpu_interface.h`, deliberately not made as part of the build/docs work.

## 10. `RelWithDebInfo` silently dropped optimisation from -O3 to -O2

The pre-CMake-rewrite build hard-appended `-O3` to `CMAKE_CXX_FLAGS` for every configuration.
Moving to proper build types meant `RelWithDebInfo` -- what the `dev` preset uses -- became CMake's
stock `-O2`, a silent slowdown against what this code was developed and profiled against.

**Resolved.** `CMAKE_CXX_FLAGS_RELWITHDEBINFO` now defaults to `-O3 -g -DNDEBUG`, overridable.

## 11. Two examples carried paths that did not survive the directory rename

- `50-terrain-generator` looked for tree assets at `../MeshVoxelizer/trees`. Corrected to
  `../mesh_voxelizer/trees` (the build-tree sibling). The assets themselves are version 1 and will
  not load either way; the example warns clearly and renders bare terrain.
- `30-renderers` had its camera hardcoded to `(1018, 413, -330)`, a position tuned for the Sponza
  scene. With `--scene` now accepting anything, that meant every other scene opened inside the
  geometry or in the void. **Resolved** by porting the automatic framing from `10-scene-previewer`.

It also printed the camera position via `core::warn` on **every frame** -- leftover debugging that
made the log unreadable. Removed.

---

## 12. A killed build leaves zero-byte objects that survive the next build

Killing a compiler mid-write (`pkill cc1plus`, a hard Ctrl-C, an OOM kill) can leave a **zero-byte
`.o`**. Its timestamp is newer than its source, so make considers it up to date and never rebuilds
it -- and `ar` will happily archive an empty object.

The symptom is a link error that makes no sense: an undefined reference to a function that is
plainly declared in the header and defined in the `.cpp`, with matching signatures.

```
undefined reference to `projv::utils::loadComposeFromDisk(std::string const&)'
```

Check for it before doubting the code:

```bash
find build -name '*.o' -size 0
```

Deleting the empty objects is enough; the next build recompiles them. A full `rm -rf build` also
works but is not necessary.

Seen in this repository after a build was killed with `pkill`: three objects were truncated
(`compose_io.cpp.o` and two Lua translation units), and the resulting `libprojectV.a` was missing
every symbol from `compose_io.cpp` while containing 173 other `projv::utils::` symbols.

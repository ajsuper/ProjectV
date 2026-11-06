# Known latent issues

Real defects that are not currently triggered, found while packaging the engine
(see the tooling/packaging plan). Each is recorded here rather than fixed in place
because fixing it is a change of its own scope.

---

## 1. `assignSystemStage` captures a dangling reference — **resolved**

The old `assignSystemStage` stored `app.Startup = [=, &app]() { system(app); };` -- a lambda that
captured `app` by reference and lived inside `app` -- so an `Application` could not be moved,
copied or returned from a factory without every stage pointing at the old object.

**Fixed by the runtime spine (`14678cb`).** `core/ecs.h` and `assignSystemStage` are
gone. `projv::Application` (`core/application.h`) stores `std::function<void(Application&)>`
systems and passes itself in when it runs them, so nothing captures it. It is also deliberately
neither copyable nor movable: it keeps pointers to its own `Time` and `Events` and connects a
`CloseRequested` handler to itself. `tests/unit/test_application.cpp` `static_assert`s both.

---

## 2. `core::perf` is unqualified in two examples' profiler blocks

`docs/examples/PathTracer/main.cpp:269` and `docs/examples/edit_demo/main.cpp:467` (now
`examples/30-renderers/main.cpp` and `tests/manual/edit_demo/main.cpp`) call
`core::perf(...)` where the enclosing scope has no `core` — it needs `projv::core::perf`,
which is what `ScenePreviewer` and `terrain_generator` correctly use.

Both sites sat inside `#if defined(PROJV_ENABLE_PERF)`, and neither example's Makefile defined
that macro, so the block was never compiled and the error never surfaced.

**Resolved.** It surfaced on the first build after the examples moved to the unified CMake
build, which puts the log definitions on the target with `PROJV_LOG_PERF` on by default —
exactly as predicted here. Both are now `projv::core::perf`.

---

## 3. Public headers leak implementation dependencies

`<iostream>` is included by eight public headers (`graphics/`: `disk_io.h`, `gpu_interface.h`,
`manage_resources.h`, `perform_renderer.h`, `render_instance.h`, `type_mapping.h`; `utils/`:
`voxel_math.h`, `voxel_management.h`); `nlohmann/json.hpp` by six (`graphics/disk_io.h`,
`runtime/entities.h`, `runtime/loading.h`, `runtime/net.h`, `runtime/physics.h`,
`utils/attachments.h`); and `render_instance.h` pulls in `GLFW/glfw3.h` and `glfw3native.h`.

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

The loader accepts container versions 2 to 4 and rejects version 1 with a clear message, then
skips the block. A scene whose every `.data` is v1 therefore *parses* — its `compose.json` is
fine, its components are counted — and contains no geometry at all.

Nine bundled `.data` files are still v1, about 210 MB (fourteen, 250 MB, before the trees went):

| Asset | Consequence |
|---|---|
| `examples/30-renderers/SponzaScene/model.data` | **The renderer gallery renders an empty scene.** All six renderers start, submit frames and show nothing. (`40-advanced-renderer` no longer embeds it; it takes a scene as an argument.) |
| `examples/30-renderers/QuadSponza/sponza_{a,b,c,d}/model.data` | Same, for the four-instance scene. |
| `tests/manual/edit_demo/SponzaScene/model.data` | Same. |
| ~~`MeshVoxelizer/trees/*` (8 assets)~~ | **Resolved:** removed. The terrain generator's trees are now grown by the scene editor's tree brush (`50-terrain-generator/biomes/<biome>/assets/`, current format). |
| `examples/10-scene-previewer/scenes/{Sibenik,LostEmpire}/model.data` | Already documented in that example's README. |
| `examples/10-scene-previewer/scenes/StonehillCastle/model.data` | Unreferenced leftover; that scene loads from its v2 components. |

This is pre-existing content rot — the files have not changed since 2026-08-01 — and it is
invisible from the outside because nothing fails loudly. An example that starts, renders and
exits cleanly looks fine; only the chunk count in the log says otherwise.

Fixing it means re-voxelizing from the source meshes, which are not in the repository. Until
then, "the example runs" is not evidence the example works: check the `Loaded N chunk(s)` line.

## 6. `editing_p1` fails 6 of its 86 self-checks

**It does not currently compile** ([#16](#16-editing_p1-and-edit_demo-no-longer-compile)), so this is the
last result it gave.

Against a scene that loads (`examples/10-scene-previewer/scenes/SmallVox`), the driver reports 80 `[OK]` and
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
  `../mesh_voxelizer/trees` (the build-tree sibling); the assets there were version 1 and did not
  load. **Resolved:** the example now has its own trees (`50-terrain-generator/biomes/<biome>/assets/`, staged beside
  the binary), grown by the scene editor's tree brush through `terrain_asset_baker`.
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

**Also (2026-11-02):** a machine that went down mid-build left zero-byte **dependency files** --
`*.o.d` and the link step's `link.d` -- and CMake's dependency scan aborts on them, not make:

```
cmGccStyleDependency ... Assertion '!this->empty()' failed.
```

`find build \( -name '*.o.d' -o -name 'link.d' \) -size 0 -delete` (with the `.o` beside each `.o.d`)
clears it. Do not delete `compiler_depend.make` by hand: configure writes it, and the build fails without
it until the next `cmake --preset dev`.

---

## 13. `50-terrain-generator` keeps its own chunk free list

Found while writing the docs (2026-10-15). The streamer creates chunk rows with
`scene.chunks.push_back` and recycles them through its own `freeChunkSlots`
(`examples/50-terrain-generator/main.cpp`, around line 2583), instead of `utils::allocateChunkSlot`
and `Scene::slots`. It works because nothing else in that example frees chunk rows, but it skips the
engine's free list and its `headerDirty`/`chunkID` setup, and it is the pattern AGENTS.md says not
to copy. Fix: allocate through `allocateChunkSlot` and drop `freeChunkSlots`.

## 14. Turning `PROJV_LOG_MINIMAL` off does not bring the categories back

`PROJV_LOG_MINIMAL=ON` writes `PROJV_LOG_TRACE/PERF/EDIT/RENDER/INFO` into the cache as `OFF` with
`FORCE` (top-level `CMakeLists.txt`). Reconfiguring with `PROJV_LOG_MINIMAL=OFF` leaves them `OFF`.
Workaround: pass the categories explicitly (`-DPROJV_LOG_INFO=ON ...`) or delete the cache. Fix:
derive the effective set without writing the user-facing options.

## 15. `ConstantVelocity`'s spin calls libm, which feeds a kinematic pose

`runtime/physics.cpp` turns a kinematic body flying on a `ConstantVelocity` with
`glm::angleAxis(turn, axis)` (about line 2471), which calls `sin` and `cos`. Those can differ in the
last bit between glibc and the Windows C runtime, and the result is the pose given to Jolt each step,
so a spinning body on Linux and on Windows would drift apart. The rule is that nothing feeding a step
calls libm ([windows.md](windows.md), D4; the same cause was found in a strike's `cbrt` and fixed). Not
hit by the golden recording, which has no spinning kinematic body, so nothing checks it. Fix: a
quaternion from an angle built from `+ - * /` only (a Taylor series, or Jolt's deterministic `Sin`/`Cos`
reached through `PhysicsWorld`), and a golden session that spins one.

## 16. `editing_p1` and `edit_demo` no longer compile

Found 2026-10-30, building the manual tests (`-DPROJV_BUILD_MANUAL_TESTS=ON`) during shader-library S2.
Both still read a `GeometryBlob`'s `materialIDs` and `geometry` vectors, which the tile-table storage
replaced ([voxel-storage-tiles.md](voxel-storage-tiles.md)); `edit_demo` also uses them in its own
code (`main.cpp:264`, `:325`). Nothing in the default build compiles them, so nothing noticed. Fix: read
and write through `utils/blob_voxels.h` as the engine does. `exit_path` is unaffected.

## 17. One hit in the physics example's first demo wakes every block, and the frame stalls

Found 2026-11-02 by hand, with the dev layer's Physics panel open (`./physics`, first demo: two towers
and a pyramid of blocks). Hitting one box sets every block awake -- the panel's awake count jumps to
the whole stack -- though most of them do not move, and the frame takes a large spike while they are.
Not yet looked into. Candidates: the blocks resting on one another form a single island, so one
contact wakes all of it (Jolt's islands, as designed, but then the stack should fall back asleep
quickly); or the engine wakes bodies itself (a break, an impact-load pass, `teleport`/`setVelocity`
on a group) more widely than the hit. Reproduce with the Physics panel's awake count and step time;
`DEMOS_ONLY=1` for that demo alone.

## 18. A scene's GPU data cannot be replaced under a running renderer

Found 2026-11-02 in the island demo (`examples/97-island-demo`, docs/plans/island-demo.md, I4). Its start
screen drew an empty scene, then loaded the chosen world into the same `Scene`, called
`graphics::destroyGPUData` on the old `GPUData` and `createTexturesForScene` for the new, and drew on
with restir-pt. A few seconds in, twice in two tries, the process died in the NVIDIA driver (the stack in
\#25, after a fault). Making the `GPUData` once, before the first frame, has never failed. Not
investigated; the likely cause is that a renderer (restir-pt at least) keeps the handles of the textures it
was first given -- in its uniforms or samplers -- and draws with them after they are destroyed. The demo
now restarts the program into the chosen world. Fix: either renderers re-bind the `GPUData` they are handed
each frame (and the engine says replacing it is supported), or the engine says plainly that a `GPUData` is
made once per renderer. Reproduce: the island demo before commit `f3dd8d6`, a world chosen on its start
screen.

## 19. Systems cannot be uninstalled

`Application::addSystem` has no inverse, and `installPhysics`, `installSceneBridge`, `installUi`,
`installImpactFracture` add systems for good, with no teardown for the state they keep in `world.ctx()`.
So a program cannot leave one world and enter another -- a "back to the title screen" -- without
restarting; the island demo restarts itself (#18 is the other half of why). Fix: an uninstall per
install (or a way to reset the physics world and the bridge for a new scene), and a way to remove a
system by name.

## 20. Cheap debris does not say which voxels it took

`BodyBroken` lists every body piece's voxels (`pieceVoxels`, in the broken component's voxel space) and
the dust's (`dust`), but the pieces made cheap debris only as entities (`debris`), with no voxels. A game
that must react to exactly what a break removed -- the island demo clears the plants standing on removed
ground -- cannot for a break that became debris, which with `PhysicsConfig::debris` on is most of a
strike's shards. The demo looks in a sphere round `BodyBroken::at` instead. Fix: `debrisVoxels` beside
`pieceVoxels` (or one list for all pieces, debris marked), filled where `pieceVoxels` is
(`src/runtime/physics.cpp`, the break pass).

## 21. No physics material that does not collide

Every palette entry with a physics material collides; one without collides with its body's own values.
So decoration that should be passed through -- grass, flowers, reeds -- cannot live in the ground's grid,
and the island demo keeps it in a second component with no collider. Then a break of the ground does not
touch it, and the demo clears floating plants by hand (#20). Fix: a material flag (`"collides": false`)
that leaves those voxels out of the collision shapes and the support pass, and lets a break take them with
the ground.

## 22. A strike's reach under half a voxel breaks nothing, silently

`runtime::Strike::reach` is in metres; the fracture turns it into voxels and returns at once if that is
under 0.5 (`src/runtime/physics.cpp`, about line 877: "not enough to break a voxel"). The header says
nothing of it, and nothing is logged, so a strike meant to break one voxel -- the island demo's chisel, at
0.45 voxels -- breaks nothing and looks like a bug elsewhere. Fix: say it in `Strike::reach`'s comment
(and that between 0.5 and 1 voxel breaks just the struck one), or log the refusal once.

## 23. Support is flooded through edges and corners, and the docs do not say so

The support pass (`localSupport`, `src/runtime/physics.cpp`) joins voxels through faces, edges and
corners. `StaticCollider::splits` says "face-connected island". So two things that only touch at an edge
or a corner hold each other up: the island demo's trees, crowns touching diagonally, stood when a trunk was
cut through, until the generator kept a one-voxel gap round each tree in all 26 directions. Fix: correct
`splits`' comment (and `BodyBroken`'s, which also says face-connected), or flood through faces only if
that was the intent.

## 24. Pieces of a static collider keep its link policy

A piece that breaks off is linked with the broken entity's `OnUnlink` (`src/runtime/physics.cpp`, about
line 1011), which for a static collider is usually the default `Keep`. Destroying such a piece's entity
leaves its voxels in the scene, drawn and orphaned. Cheap debris, by contrast, is always linked
`OnUnlink::Destroy`. A game that removes pieces (the island demo gathering rubble) has to delete the
component itself (`utils::deleteComponent`) for one kind and not the other. Fix: pieces linked
`OnUnlink::Destroy` (they are made by the break, owned by nothing else), or say in `BodyBroken` which
policy pieces get.

## 25. bgfx's Vulkan frame timer crashes now and then on the NVIDIA driver

`bgfx::vk::TimerQueryVK::begin`, called from `RendererContextVK::submit`, segfaults inside
`libnvidia-glcore.so.580.119.02` in about one run in five, on any program, after start-up, with no
profiler on (the dev layer's `BGFX_DEBUG_PROFILER` is not the cause: this is bgfx's always-on frame
timer). Seen across many builds and programs (the advanced renderer, the island demo). The same driver
also kills runs on exit, after a capture (a segfault or a floating-point exception in `SwapChainVK::present`).
Not the engine's code, but every program pays for it: the island demo autosaves every minute because a
crash saves nothing. Fix: a bgfx upgrade, a patch that lets the frame timer be turned off (as the
Jolt submodule carries one, `cmake/patches/`), or a report upstream.

## 26. The physics example's walker checks depend on how F's blow happens to break

Demo 11's self-test sends a walker at "the biggest piece F threw past the wall" and expects it to be
stopped or to climb. Which pieces F makes depends on the session's step timing: windowed (steps paced
by the real clock) the piece is big enough, but `./physics --headless` (one step a frame) leaves a
32-voxel piece, which the walker shoves 2.8 m along, so the check fails, before and after the
2026-11-05 support change alike. "F's shards went on through the wall" (more than 60% of them) is as
borderline: 50-60% in some windowed runs. Fix: build the walker's obstacle and the shards' target
for the test (a piece of known size, a strike of known energy) rather than picking from what F made.

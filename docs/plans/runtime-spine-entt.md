# Runtime spine: EnTT, a scheduler, time, events, and the Scene bridge

Status: **P1 through P4 implemented** on branch `runtime-spine` (off `component-attachments`),
uncommitted. P5 stays unscheduled, as planned. See **As built** at the end for where the code
differs from the decisions below. Prerequisites, done: C++20 (`a870fea`), the unit-test suite, and
component attachments with `op` moved out of the engine (`65faef9`).

**What changed in this revision, and why:**
- **D0 is done.** It is condensed to a record.
- **Spawning reads attachments through their types** (D6). The attachment API that now exists is
  typed (`AttachmentTraits<T>`), so spawn handlers are registered by type, not by key string, and
  folder-level (document-scope) attachments get handlers of their own.
- **New D9: the engine needs a component delete.** Today only the editor can delete a component
  (`deleteComponent` in its `main.cpp`). A deleted component is marked by renaming it to
  `"__deleted__"`; the engine checks that once (`compose_io.cpp:1271`) and the editor checks it in
  41 places. The bridge's `OnUnlink::Destroy` and the `ComponentDestroyed` event need deletion to
  exist below the editor.
- **New D10: where the bridge lives.** It needs both EnTT and `Scene`, and D1 forbids `utils` from
  depending on EnTT. So it becomes its own layer.
- **D2 keeps `Scene`'s copy members.** The attachment store copies through `std::any` with no help.
  The unit tests copy a `Scene` and rely on it. Removing the copy members no longer buys anything.
- **D8 is corrected.** The repo has no `vcpkg.json`, so "add `entt` to the manifest" was wrong. The
  wiring now mirrors how `tests/unit/CMakeLists.txt` already finds doctest.
- **Threading is a stated constraint, not an open question.** A typed attachment read can decode
  raw entries in place, even through a `const Scene&`. Attachment reads, spawn handlers included,
  are therefore main-thread only.

## What this is

ProjectV's renderer is mature; its runtime is not. Before physics or networking can be built, the
engine needs the thing they both sit on: entities that can be created and destroyed, more than one
system per stage, a clock, a way for systems to talk without knowing about each other, and a defined
relationship between an *entity* and a voxel *component* in `Scene`.

This plan replaces `core/ecs` with [EnTT](https://github.com/skypjack/entt) and builds a thin,
ProjectV-owned layer on top: the application loop, the scheduler, time, input, events, and the
Scene bridge. EnTT provides storage and queries; ProjectV owns the policy.

---

## Where we are today

`core/ecs` (`include/core/ecs.h`, `src/core/ecs.cpp`, `include/data_structures/application.h`):

| Problem | Consequence |
|---|---|
| `assignSystemStage` *assigns* `app.Update = ...` | One system per stage. A second registration silently replaces the first. |
| Stage lambdas capture `&app` and are stored in `app` | `Application` cannot move (known-latent-issues #1). |
| `runApplication` is `while(!close){ Update(); Render(); }` | No delta time, no fixed step. hello_voxel's camera moves `0.4` units **per frame**, so its speed depends on frame rate. |
| No `destroyEntity`; IDs are a bare counter | Entities can only be created. |
| `ComponentStorage<T> = unordered_map<Entity,T>` inside `std::any` | Every `forEachEntityWith` does one hash lookup per component per entity. |
| `getComponent` calls `getOrCreateStorage` | Reading a missing type creates its storage, then `.at()` throws. |
| `getGlobalResource` warns on a miss, then `any_cast`s the end iterator | Undefined behaviour after the warning. |
| No events | Systems can only talk through shared global resources. |

**Usage across the repo:** no example or test creates an entity or an ECS component. The entity
half of the API has no callers. What *is* used:

- `createApp` / `assignSystemStage` / `runApplication`: 6 examples and 2 manual tests, 29
  `assignSystemStage` calls.
- `createGlobalResource` / `getGlobalResource`: in every example, for `Scene`, `GPUData`,
  `RenderInstance`, `Camera`, `EditorState`, and so on.

That makes migration cheap. We are replacing a resource bag and a four-slot loop, not porting a
running ECS.

**What already exists to build on:**
- `tests/unit/` with doctest and `ctest --preset dev`. Every phase below adds its tests there.
- `utils/attachments.h`: per-component and per-folder program data, saved in compose.json. It is
  the input to spawning (D6).
- `utils::setComponentTransform` → `rebakeSubtree`: moving a component is header writes only. This
  is the bridge's sync path, unchanged.

---

## Decisions

### D0. C++20 — done (`a870fea`)

Every engine, example and test translation unit compiles under `-std=c++20`. The only change needed
was two `disk_io.cpp` log calls that passed a runtime format string, which fmt now checks at compile
time. bgfx.cmake keeps its own C++17. **Not C++23**, because Apple Clang's library support lags and
nothing here needs it.

What C++20 buys this plan:
- EnTT 4 requires it.
- Designated initializers for `Time` and event structs.
- Concepts, which can later turn known-latent-issues #9 (`setUniformToValue` rejecting `const`
  arguments at runtime) into a compile error.

### D1. EnTT, exposed rather than wrapped

`projv::World` becomes an `entt::registry`. Views, groups, `emplace`, `get`, `try_get`, signals and
snapshots are used directly, as EnTT names them.

**Why not wrap it.** EnTT's value lies in exactly the surface a wrapper would hide: typed views,
`each` with structured bindings, storage signals, and groups. A `projv::forEach<A,B>` facade would
either cover all of that, which means re-documenting EnTT, or cover part of it, so users reach past
it anyway. Wrappers are worth it when a backend might be swapped. We do not plan to swap this one.

**What ProjectV does own.** The loop, the stage order, `Time`, `Input`, event pumping, and the Scene
bridge. These are the policy decisions, and EnTT deliberately leaves them open.

**Against.** `core.md` says core has no dependencies. EnTT is header-only and as general-purpose as
`core` itself, so the spirit of that rule holds: core still knows nothing about voxels or graphics.
Update `core.md` to say "no dependencies beyond EnTT and glm".

**A names header, not a wrapper.** `core/world.h` declares `using World = entt::registry;` and
`using Entity = entt::entity;`. Engine signatures say `World&`, so an EnTT upgrade that moves a type
changes one header, not every signature.

**Pinned.** EnTT is a submodule at a release tag (v4.0.0) and is upgraded deliberately, in a commit
of its own with the unit tests run, the way bgfx is.

**Scope boundary.** `utils` (scene_query, compose_io, attachments) does **not** depend on EnTT. The
voxel layer and its file formats stay usable by tools with no runtime: the mesh voxelizer, a bake,
or the unit tests that load and save compose folders. D10 is where the two meet.

### D2. Resources move into `registry.ctx()`

`entt::registry::ctx()` is EnTT's built-in global-resource store, and it replaces
`World::globalResources` directly.

```cpp
auto& scene = world.ctx().emplace<projv::Scene>();
auto& scene = world.ctx().get<projv::Scene>();      // asserts in debug on a miss
auto* scene = world.ctx().find<projv::Scene>();     // nullptr on a miss
```

The old `createGlobalResource` / `getGlobalResource` stay for one release as one-line forwarding
shims marked `[[deprecated]]`, so examples migrate in their own commits. `getGlobalResource` fails
loudly: `core::error` plus abort, never the UB it has today.

**`Scene` keeps its copy constructor and copy assignment.** The earlier version of this plan removed
them, because `std::any`-based resources were their only reason to exist. They now have another:
the attachment store copies with them, and `tests/unit/test_attachments.cpp` copies a `Scene` to
check exactly that. Rewrite the comment above them to say so, and drop its mention of
`createGlobalResource`. All four special members must still list every field
(component-attachments A6).

### D3. A fixed stage list, ordered systems inside each stage

```
Startup                         once
loop:
  PreUpdate      poll platform, fill Input, advance Time
  FixedUpdate    0..N times per frame at Time::fixedDelta   (physics lives here)
  Update         gameplay, once per frame, variable dt
  PostUpdate     transform propagation, Scene bridge sync, event pump
  Render         draw
Shutdown                        once
```

```cpp
struct Application {
    World world;
    bool  closeRequested = false;
    void  addSystem(Stage, std::string name, System fn);   // appends; order = registration order
    void  run();
    Application(const Application&) = delete;               // pinned: fixes latent issue #1
    Application(Application&&)      = delete;
};
using System = std::function<void(Application&)>;           // the app is passed in, never captured
```

- **Registration order is execution order** within a stage. It is explicit and readable, and it is
  what every current example already assumes.
- **Systems are named**, so a profiler line or a crash report can say which one.
- **Not `entt::organizer` yet.** The organizer builds a dependency graph from declared component
  access, which only pays off with a multithreaded executor. Revisit when one is wanted, and see
  the threading constraint under Open questions.
- **`Application` is non-movable by construction.** That closes latent issue #1 instead of
  documenting it. `createApp()` becomes a constructor. Examples change
  `Application app = createApp();` to `Application app;`.

The old `assignSystemStage` stays for one release as a shim that calls `addSystem`, mapping the old
four stages onto the new ones.

### D4. `Time` is an engine resource

```cpp
struct Time {
    double  elapsed;          // seconds since Startup, scaled
    float   delta;            // this frame, scaled, clamped to maxDelta
    float   unscaledDelta;
    float   scale = 1.0f;     // 0 pauses gameplay and FixedUpdate; UI still sees unscaledDelta
    float   fixedDelta = 1.0f / 60.0f;
    float   maxDelta   = 0.25f;     // a debugger pause does not become a 30-second physics step
    int     maxFixedStepsPerFrame = 8; // spiral-of-death guard; the excess is dropped and reported
    float   fixedAlpha;       // accumulator / fixedDelta, for render interpolation
    uint64_t frame;
    uint64_t fixedStep;
};
```

Physics depends on this. Without `fixedAlpha` a 60 Hz simulation stutters on a 144 Hz display.
Dropped fixed steps are reported through `core::perf`, not swallowed silently.

The clock is injectable (a `std::function<double()>` defaulting to `steady_clock`), so the unit
tests can drive `Time` with exact deltas instead of sleeping.

### D5. Events: EnTT's dispatcher, queued by default, pumped at one point

`entt::dispatcher` lives in `ctx()`. ProjectV fixes the policy:

- **`enqueue` is the default.** Events are delivered when the dispatcher is pumped, once per
  frame at the end of `PostUpdate`. A system never has its handlers run in the middle of its own
  body, and an event sent anywhere in a frame is seen by every handler in the same frame.
- **`trigger` (synchronous) is allowed** for engine-internal signals that must not lag a frame.
  Each such use gets a comment explaining why.
- **Events raised by handlers** during the pump are delivered next frame, not recursively. This
  bounds the pump and makes cycles visible instead of turning them into hangs.
- **Engine-defined events** to start with: `WindowResized` and `CloseRequested` (P2), then
  `EntityLinked`, `ComponentDestroyed` and `ComponentTransformChanged` (P3, which needs D9 for
  the second). Physics adds `CollisionBegan` / `CollisionEnded` later.

`CloseRequested` replaces the poll every example does today
(`if (renderInstance.shouldClose) app.closeAppFlag = true;`). The default handler sets
`closeRequested`, and an app can register its own handler to veto it, for an unsaved-changes
dialog for example.

**Storage signals are not events.** `registry.on_construct<T>()` and friends fire synchronously on
mutation, and the Scene bridge uses them (D6). They are a different tool, and gameplay should not
treat them as a message bus.

### D6. The Scene bridge: two levels of hierarchy, one linking rule

**The decision (H4):** the **Scene hierarchy** describes what an object is made of: its parts,
nested assets, and placements within an asset. It stays in `Scene` and in compose.json, unchanged.
The **entity hierarchy**, when it arrives, describes what is attached to what *between* objects:
a gun in a hand, a camera on a ship. Entities **link** to Scene components and never absorb them.
Appendix A records why H2, where entities are the scene tree, was rejected.

Since `65faef9` the Scene hierarchy means **placement only** to the engine: `op` is the editor's
attachment, and the engine never folds anything. The bridge needs no rules about boolean
operands.

#### The link

```cpp
struct Transform      { vec3 position; quat rotation; float scale = 1.0f; };  // local
struct WorldTransform { mat4 matrix; };                                          // derived cache

enum class LinkMode   { Root, Part };
enum class OnUnlink   { Keep, Destroy };
struct VoxelComponent { ComponentHandle handle; LinkMode mode = LinkMode::Root;
                        OnUnlink onUnlink = OnUnlink::Keep; };
```

- **Root link** (the default) targets a component with no Scene parent. The entity's *world*
  transform is the root's local transform, so the two hierarchies cannot disagree. Linking a
  non-root component in Root mode is an error, reported when the link is made.
- **Part link** targets a component inside an asset, for example a door in a house. `Transform`
  is then measured in that component's **Scene-parent space**, and the entity **may not** have an
  entity parent. Without this mode, gameplay could never animate a sub-part.
- **A link must target a live component** (`utils::isComponentAlive`, D9). Linking a deleted one
  is an error, reported when the link is made.
- **Authority.** The entity's `Transform` wins. The editor gizmo becomes one more writer of
  `Transform`. A change to the record made *not* through an entity (compose_io during a load, for
  example) is reconciled by re-seeding the linked entity's `Transform` from the record and firing
  `ComponentTransformChanged`.
- **On link** (`on_construct<VoxelComponent>`): validate liveness and the mode, then seed
  `Transform` from the record's `localPosition`/`localRotation`/`localScale`.
- **On unlink or destroy** (`on_destroy<VoxelComponent>`): `Keep` leaves the component in the
  Scene; `Destroy` calls `utils::deleteComponent` (D9), which also clears its attachments.
- **The component is deleted while still linked** (by the editor, or by gameplay through D9):
  `deleteComponent` reports it, the bridge enqueues `ComponentDestroyed`, and the linked entity
  loses its `VoxelComponent` (`Keep` semantics, since the component is already gone). The entity
  itself survives. Whether it should be destroyed is the game's decision, made in a
  `ComponentDestroyed` handler.

#### The frame (PostUpdate, in this order)

1. **Propagate** entity `WorldTransform` from the entity roots down. A no-op until entity parenting
   exists (P5).
2. **Sync**: for each entity whose `Transform` or `WorldTransform` changed this frame, call
   `utils::setComponentTransform` on its linked component. A Root link writes its world transform;
   a Part link writes its local `Transform`. Change tracking uses `registry.patch<Transform>` with
   an `entt::observer` (or an explicit `TransformDirty` tag), so an untouched entity costs nothing.
3. `setComponentTransform` → `rebakeSubtree` writes chunk and grid headers and sets
   `headerDirty`. That is the existing path and needs no change. It is cheap: header writes, no
   voxel work.
4. **Pump** the event dispatcher (D5).

#### Sockets (with entity parenting, P5)

```cpp
struct Attachment { entt::entity parent; ComponentHandle socket = INVALID_COMPONENT_HANDLE; mat4 offset; };
```

An entity can follow a parent entity, or a specific **Scene component inside** the parent's linked
asset, such as a light at the tip of a voxel lamp-post part. A socket *reads* that component's
world transform (`utils::getComponentWorldMatrix`) and never writes to the Scene, so it cannot
conflict with the Scene hierarchy. Only unlinked or Root-linked entities may have an entity parent;
Part-linked entities may not (see above).

> **Naming:** this ECS component was called `Attachment` before component attachments existed. Name
> it `ParentLink` (or `Follow`) when it is built, so "attachment" means one thing in this codebase:
> program data on a scene component.

Entity parenting is built as a relationship component (parent, first child, next sibling).
Sibling order does not matter at the entity level, unlike Scene children.

#### Spawning from attachments

> **Superseded** by `authored-entities.md`: gameplay data is now ECS components saved as themselves
> in `entities.json` beside compose.json, linked by persistent component id. The spawn-handler API
> below was built, then retired.

Attachments are what an asset says; ECS components are what the running game is doing. Spawning
turns the first into the second, through the typed API that now exists:

```cpp
// A module registers one handler per attachment type it understands. The key comes from
// AttachmentTraits<T>::key, so handlers and the file can never disagree about the spelling.
template<typename T>
void registerSpawnHandler(World&, std::function<void(World&, entt::entity, const T&)>);

// Folder-level data (AttachmentScope::Document): a level's gravity, a spawn table. Run once per
// spawned root, against the entity that links the root.
template<typename T>
void registerDocumentSpawnHandler(World&, std::function<void(World&, entt::entity, const T&)>);

// One entity per live root component, Root-linked, each spawned.
std::vector<entt::entity> spawnFromCompose(World&, Scene&);
// Link one existing component and run its spawn handlers.
entt::entity spawnComponent(World&, Scene&, ComponentHandle, LinkMode = LinkMode::Root);
```

- **When a link is made,** the bridge asks each registered type for its attachment on the linked
  component (`utils::getAttachment<T>`) and calls the handler for each one present. For a
  Root link it also checks document-scope attachments: on the component, if it is an Asset
  standing for a folder, and on `INVALID_COMPONENT_HANDLE`, the folder that was opened.
- **Keys with no registered handler are ignored.** They stay in the Scene and are saved unchanged,
  which is the attachment system's own guarantee and costs the bridge nothing.
- **Handlers read; they never write attachments back.** Runtime state goes into ECS components.
  Saving game state is a separate format (Open questions, 2).
- **Main thread only.** `getAttachment<T>` can decode raw text in place, even through a
  `const Scene&`. Spawning runs in a stage on the main thread, never inside a parallel job.
- **Example:** `mygame.spawn` → `Spawner`; later `projv.physics.body` → `RigidBody` + `Collider`.

**What this enables:** a physics body is an entity with `Transform`, a Root `VoxelComponent`,
`RigidBody` and `Collider`. Physics writes `Transform` in `FixedUpdate`, the bridge syncs it in
`PostUpdate`, and the renderer draws it. Nothing in physics has to know about chunks.

### D7. Input is a platform resource, not core

GLFW is reached from `graphics::RenderInstance`, and `core` must not depend on it. Add an `Input`
resource, filled in `PreUpdate` by a system the graphics module registers:

```cpp
struct Input { /* key/mouse button state: down, pressed-this-frame, released-this-frame;
                  cursor pos + delta; scroll delta; cursor capture flag */ };
```

The key enum is ProjectV's own, mapped once from GLFW. This also moves `glfwPollEvents` out of
`renderConstructedRenderer`, where it is buried today (`perform_renderer.cpp:299`): polling is an
input concern and belongs at the top of the frame.

Examples stop calling `glfwGetKey` directly. ImGui keeps its own GLFW backend, and `Input` reports
`wantCaptureKeyboard/Mouse` so gameplay can yield to it.

### D8. Dependency wiring

The same shape `tests/unit/CMakeLists.txt` already uses for doctest: `find_package` first, then a
header-only `INTERFACE` target over the pinned submodule. EnTT 4's own CMake requires 3.28 and ours
requires 3.20, so the submodule path does not `add_subdirectory` it.

```cmake
find_package(EnTT CONFIG QUIET)
if(NOT TARGET EnTT::EnTT)
    add_library(projv_entt INTERFACE)              # external/entt, pinned to a v4.0.x tag
    target_include_directories(projv_entt SYSTEM INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/entt/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/projv/third_party>)
    add_library(EnTT::EnTT ALIAS projv_entt)
    # + install the headers and export projv_entt, the way the other vendored deps install
endif()
target_link_libraries(projectV PUBLIC EnTT::EnTT)
```

- `ProjectVConfig.cmake.in` gains `find_dependency(EnTT CONFIG)`.
- **There is no `vcpkg.json` in the repo.** The `vcpkg` preset finds whatever the toolchain
  provides, and nothing records the package list, for any dependency. `entt` and `doctest` join
  bgfx, glm, spdlog, nlohmann-json and glfw3 on an unwritten list. Adding a manifest that lists all
  of them is a small, separate fix. It is recommended before anyone else builds with the preset,
  but it is not part of this plan.
- `ENTT_ASSERT` is left as EnTT's default (`assert`). Rerouting it needs a config header included
  before every EnTT include in every consumer, or the build violates the one-definition rule. Not
  worth that until there is a reason.

### D9. The engine can delete a component (new)

Today the engine can create a component (`utils::addComponent`) but not delete one. Deletion is the
editor's `deleteComponent` (`examples/60-scene-editor/main.cpp`). It is about 80 lines that:
- detach the component from its parent;
- kill each chunk and release its geometry blob;
- empty a grid's cells;
- rename every node in the subtree to `"__deleted__"`;
- clear attachments;
- clean up the editor's selection.

The bridge needs this below the editor, for `OnUnlink::Destroy` and for `ComponentDestroyed`. So
does any gameplay that removes a placed object.

```cpp
// scene_query.h
void deleteComponent(Scene&, ComponentHandle);          // whole subtree; tombstones, never erases
bool isComponentAlive(const Scene&, ComponentHandle);   // false out of range or tombstoned
// Called once per tombstoned handle, leaves first. The bridge registers one; the editor needs none.
void setComponentDeletedCallback(Scene&, std::function<void(ComponentHandle)>);
```

- **Promote the editor's version, do not rewrite it.** It already handles the two cases that bit:
  grids, whose cells are not in `looseChunks`, and descendants, which have to be renamed too. The
  editor's own `deleteComponent` keeps its selection cleanup and calls the engine for the rest.
- **The tombstone stays a name, for now.** A `bool alive` field would be cleaner. But the editor
  reads the name in 41 places, and `saveComposeToDisk` skips records by it.
  `isComponentAlive` is the one engine-side reader, so changing the representation later touches
  one function plus the editor.
- **Handles are still never reused.** `addComponent` always appends, so a stale handle in a
  `VoxelComponent` can be detected rather than silently pointing at a new component.

### D10. Where the bridge lives (new)

Three layers, each depending only on the ones above it:

| Layer | Depends on | Holds |
|---|---|---|
| `core` | EnTT, glm | `Application`, stages, `Time`, the dispatcher. No voxels, no graphics. |
| `utils` / `data_structures` | core's math and log, nlohmann_json | `Scene`, compose_io, attachments, `deleteComponent`. **No EnTT.** |
| `runtime` (new: `include/runtime/`, `src/runtime/`) | core, utils | `Transform`, `VoxelComponent`, the link handlers, the sync system, spawning. |

`Input` (D7) lives in `graphics`, which already owns GLFW. All of it is still one library target,
`projectV`; the layers are a rule about includes, not separate targets. Because EnTT is linked
`PUBLIC`, nothing in the build would notice `utils` starting to include it. So P1 adds a CTest check
that fails if any file under `include/utils/`, `include/data_structures/` or `src/utils/` includes
`entt/`.

---

## Phases

Each phase builds, runs every example, adds its tests under `tests/unit/`, and leaves the tree
shippable. P0 (C++20 and the test suite) is done.

### P1: EnTT, Application, scheduler, Time
- EnTT submodule and wiring (D8).
- New `Application` (D3) and `Time` with an injectable clock (D4). `World` becomes
  `entt::registry`; resources move into `ctx()` (D2).
- Deprecated shims for `createApp` / `assignSystemStage` / `createGlobalResource` /
  `getGlobalResource`.
- Delete `ComponentStorage`, `forEachEntityWith`, `getOrCreateStorage` and the rest. They have no
  callers.
- Rewrite the comment on `Scene`'s special members (D2); keep the members.
- The CTest layering check (D10): no `entt/` include under `utils` or `data_structures`.
- **Tests** (`tests/unit/test_application.cpp`, `test_time.cpp`):
  - stage order, and several systems per stage running in registration order;
  - fixed-step counts for a given sequence of deltas, including the clamp and the step cap;
  - `scale = 0` pauses FixedUpdate;
  - Shutdown runs on close: the `exit_path` regression, now automated without a window;
  - `static_assert` that `Application` is neither copyable nor movable.
- **Exit:** every example still runs, through the shims.

### P2: Events and Input
- Dispatcher in `ctx()`, pumped at the end of PostUpdate (D5). `CloseRequested` and
  `WindowResized` come from the platform layer.
- `Input` resource (D7). `glfwPollEvents` moves to PreUpdate.
- **Tests:** queued delivery within the same frame; events raised inside a handler arrive next
  frame; a veto handler on `CloseRequested` keeps the app open.
- **Exit:** hello_voxel uses `Input` and `Time::delta`, so its camera speed is in units per second.

### P3a: Component delete in the engine (D9)
- Promote `deleteComponent`, add `isComponentAlive` and the deleted callback. The editor's
  `deleteComponent` calls the engine's and keeps only its selection cleanup.
- **Tests** (`test_scene_delete.cpp`): a chunk, a grid and a nested asset each delete fully; every
  descendant is tombstoned; blobs are released; attachments are cleared; the callback fires once
  per node, leaves first; save skips the result.
- **Exit:** the editor's ASSEMBLYTEST and SAVETEST results are unchanged (they exercise delete
  heavily). Today that is 71 passes on SmallVox and 31 on Sibenik, with the two known SmallVox
  failures.

### P3b: Scene bridge (no entity parenting yet)
- `runtime/` layer (D10). `Transform`, `WorldTransform`, `VoxelComponent` with Root and Part
  modes, liveness and mode validation on link, unlink policies, `ComponentDestroyed` from the D9
  callback, the dirty-tracked PostUpdate sync, and re-seeding when the record changes outside an
  entity (D6).
- `registerSpawnHandler<T>`, `registerDocumentSpawnHandler<T>`, `spawnComponent`,
  `spawnFromCompose`.
- **Tests (CPU only, no GPU):**
  - linking seeds the transform;
  - a Root link to a non-root component is rejected, and so is a link to a deleted component;
  - moving a Root-linked entity updates the root's local transform and its subtree's world
    transforms;
  - a Part link writes Scene-parent-local space;
  - an untouched entity does not rebake;
  - unlinking respects `Keep`/`Destroy`;
  - deleting a linked component enqueues `ComponentDestroyed` and unlinks without destroying the
    entity;
  - a spawn handler runs for its type's attachment and not for others;
  - a document-scope handler sees the opened folder's block;
  - an unknown attachment key is ignored at spawn and still saved afterwards.
- **Exit:** a small new example, `15-entities`, spins and orbits a few voxel components using only
  entities, systems and `Time`, with one behaviour driven by an attachment in its compose.json. It
  stays under ~300 lines, in the spirit of hello_voxel.

### P4: Migrate the examples, then remove the shims
- One commit per example, in reading order. The scene editor goes last. `EditorState` stays one
  resource, and its edit history stays its own (the same rule attachments followed: undo is the
  program's job). Splitting the editor into entities is not part of this plan.
- Delete the deprecated shims. Fix known-latent-issues #1 and mark it resolved there.
- Update `core.md`, `utils.md` (it predates compose_io and attachments), `examples/README.md` and
  `AGENTS.md`.

### P5: Entity parenting and sockets (when the first real case needs it)
- Relationship component, `ParentLink` (with optional socket), top-down propagation into
  `WorldTransform`, and the parenting restriction on Part links.
- **Tests:** child follows parent; a socket follows a Scene sub-component and never writes the
  Scene; a Part-linked entity cannot take an entity parent.
- Not scheduled. P3b is shaped so this phase adds to it without breaking it. The Root/Part rule is
  the part that had to be settled in advance.

---

## After this: what it unblocks

- **Physics v1** gets:
  - `FixedUpdate` and `Time::fixedAlpha`;
  - `Transform`, the bridge, and collision events;
  - authored body data through `projv.physics.body` and a spawn handler.

  It needs two promotions from `engine-promotion-candidates.md` first: #4 content bounds
  (broadphase) and #20 ray/plane intersection in `core::math`. Collision tests run against tree64
  occupancy using the existing DDA. A whole asset is one rigid body. Choosing between our own voxel
  collider and an integrated rigid-body library (Jolt, for example) for dynamics gets its own plan.
- **Networking** gets `entt::snapshot` for full-state serialization and the event queue as the
  natural unit for commands and replication. It needs a stable network ID component, because
  `entt::entity` values are local to one registry. Asset structure and attachments are not
  replicated, because every peer loads the same assets. Entities and voxel edits are.
- **Runtime destruction** is a one-time voxel edit (subtract a shape through the edit queue), not a
  live CSG stack. It needs no part of the editor's boolean machinery. Removing a whole placed object
  is `deleteComponent` (D9).

## Open questions

1. **Does the editor's undo log (promotion #19) record entity operations** as well as voxel edits?
   This only matters once the editor creates entities, which is outside this plan.
2. **Saved game state.** Spawn handlers read attachments and never write them, so runtime state
   saved with `entt::snapshot` needs a format of its own, beside compose.json rather than inside
   it. Confirm the one-way rule before P3b hardens it.
3. **Threading, beyond the stated constraint.** Attachment reads are main-thread only (D6), and
   `Scene`'s palettes have their own mutex. Before `entt::organizer` or any parallel executor
   arrives, state which other resources may be touched off the main thread.
4. **`bool alive` on `ComponentRecord`.** D9 keeps the name tombstone. Replacing it means a pass
   over the editor's 41 checks. Worth doing once the editor's delete calls the engine's, since
   from then on the engine is the only place that writes the tombstone.

---

## Appendix A: why entities are not the scene tree (H2)

H2 would turn every compose node into an entity, with `ComponentRecord` keeping only voxel
payload. Rejected for these reasons, each grounded in the current code:

1. **It splits `ComponentRecord` in two instead of replacing it.** Voxel payload is dense and
   handle-indexed: `gpuData.componentPaletteOffsets[h]` is filled by one pass over handles
   (`gpu_interface.cpp:502`); `Chunk::componentHandle`, `SceneGrid::componentHandle`,
   `PickResult::component`, `editQueue`, `dataRefID`, `materialPalette`, and now the attachment
   store, all key on it. `entt::entity` IDs are recycled with version bits, and EnTT's storage is
   packed and reordered. So H2 keeps `scene.components` *and* adds a registry tree, plus
   entity↔handle maps in both directions kept in sync.
2. **Every structural walk crosses the two ID spaces.** `setComponentTransform`'s parent walk and
   `rebakeSubtree`'s recursion (`scene_query.cpp`) become registry lookups that map to a handle at
   each leaf. Longer and slower, with a new failure mode: a stale handle.
3. **`utils` would need a registry for anything structural:** load, save, set-transform, list-tree,
   instantiate, attachments. Tools with no runtime (the mesh voxelizer, a bake, the unit tests)
   would have to construct one.
4. **Structure would be stored per instance.** Geometry is shared through `GeometryBlob`
   refcounting. Under H4 a placed asset is one entity linked to its root. Under H2 ten houses are
   ten copies of every folder and part as entities, all of which networking would have to handle,
   although nothing about them changes in play.
5. **Threading.** EnTT's registry is not safe for concurrent structural changes, and H2 moves
   hierarchy edits into it.

**What H2 does better:** every sub-part is an entity for free (`emplace<Hinge>(door)` with no link
step), and a light can simply be a child of a lamp part. H4 covers both, with one extra call each:
Part links and sockets.

**When to revisit:** if, once physics and gameplay exist, most gameplay targets sub-parts and
Part links appear everywhere. Under H4 those links already provide the entity↔handle mapping H2
would need, so that switch remains possible.

---

## As built

Branch `runtime-spine`. `ctest --preset dev`: 52 unit-test cases (866 assertions) plus the
`layering` check, all passing. Two deliberate breaks were confirmed to fail tests: writing every
linked entity instead of only changed ones, and dropping unknown attachments on save.

**Files.**
- core: `core/world.h`, `core/application.h`, `core/time.h`, `core/events.h` and their `.cpp`.
- graphics: `graphics/input.h` and `platform.cpp`.
- runtime: `runtime/scene_bridge.h`/`.cpp`.
- utils: `utils::deleteComponent` and `isComponentAlive`.
- removed: `core/ecs.h`, `src/core/ecs.cpp`, `data_structures/application.h`.
- `examples/15-entities`.
- tests: `tests/unit/test_{application,time,events,input,scene_delete,scene_bridge}.cpp` and
  `check_layering.cmake`.

**Where it differs from the decisions above:**

1. **Events are ProjectV's own `projv::Events`, not `entt::dispatcher` (D5).** The dispatcher
   delivers an event raised inside a handler in the *same* pump whenever that event's type is
   published after the handler's, which breaks D5's rule that everything raised during a pump waits
   for the next one. Its sinks also take only free or member functions, not lambdas. `Events` takes
   every queue before delivering anything; it is about 150 lines, with tests for both orders.
2. **Dirty tracking is a `TransformDirty` tag, not `entt::observer` (D6).** EnTT 4 removed the
   observer in favour of a reactive mixin. The tag is set from `on_construct`/`on_update<Transform>`
   and cleared by the sync, which was the plan's stated fallback.
3. **Deletion is reported by a counter, not a callback (D9).** `Scene::deletions` increments on
   each `deleteComponent`, and the bridge re-checks its links only when the counter has moved. A
   callback stored on `Scene` would be copied with every copy of the Scene and would then report the
   copy's deletions to the original's owner. `deleteComponent` returns the removed handles, leaves
   first, which is what the editor's selection cleanup uses.
4. **`spawnFromCompose` makes a document entity (D6).** The opened folder's document-scope handlers
   run once, on an entity tagged `SceneDocument`, rather than once per root. The plan's version
   would have applied a level's settings N times. An Asset root's own folder block still goes to
   that root's entity.
5. **`linkComponent` also refuses a second link to one component.** Two writers of one transform
   would fight. A Part link may name a root, since the two spaces coincide there.
6. **`Application::frameCount` is kept.** It counts frames completed, so the first frame sees 0,
   which is what the examples' accumulation shaders are written against. `Time::frame` counts frames
   begun. `closeAppFlag` was renamed `closeRequested` everywhere.
7. **Closing is `closeOnRequest`, not a veto handler (D5).** With it true (the default), a
   `CloseRequested` event ends the app. An app that wants to ask first sets it false and handles the
   event. The platform system clears GLFW's close flag after each request, so a declined request
   can be made again.
8. **EnTT's vendored target is `EnTT::EnTT INTERFACE IMPORTED GLOBAL` (D8).** The export set names
   it the same way whichever path produced it. A submodule install ships the headers under
   `include/projv/third_party/` and a small `EnTTConfig.cmake`. Verified by installing into a temp
   prefix and building a consumer that finds ProjectV and runs a bridge-using app.
9. **Time arithmetic is double inside, float outside, with a 1e-5-step tolerance.** `fixedDelta` is
   a float, so `0.05f` is a little over 0.05 and a quarter-second frame is a little under five
   steps. Without the tolerance it ran four.
10. **Only hello_voxel and 15-entities install the platform system.** The other examples are moved
    to the new Application and `ctx()`, but keep polling in `renderConstructedRenderer` (the
    default, `pollEventsInRender`) and their own GLFW input. Moving them to `Input` is optional
    follow-up work and changes behaviour in large files, so it was left out.

**Verification beyond the unit tests:**
- Every example builds, with no new warnings (the library's older unused-parameter warnings remain).
- hello_voxel, entities, renderer_gallery (`--renderer fast`), advanced_renderer and
  terrain_generator each ran without new errors until stopped.
- scene_previewer finished a 90-frame bench and exited 0.
- The editor's self-tests match the attachments branch exactly: SmallVox 71 PASS with the two known
  failures, Sibenik 31 PASS.
- `editing_p1` (manual) reports the six failures known-latent-issues #6 already lists, unchanged.
- `exit_path` compiles against the new API.

**Found along the way, not fixed:**
- AGENTS.md named `--renderer world-cascade`, which the gallery no longer offers (corrected to
  `fast`).
- The terrain generator expects mesh-voxelizer tree assets that a fresh build does not have.
- An example's assets are staged only when the example relinks, so a data-only edit needs a
  rebuild of that example (or a manual copy) to reach `build/`.

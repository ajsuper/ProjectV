# Agent Notes

## Build Commands

Everything builds from the top-level CMake project. There are no per-example Makefiles and no
per-example shader scripts any more -- one target, one shader rule.

```bash
cmake --preset dev                        # configure (submodule deps, examples on)
cmake --build --preset dev -j$(nproc)     # build the library, the examples, and their shaders
```

**Always build in parallel** (`-j$(nproc)`, on every `cmake --build`). The presets use the Makefile
generator and set no job count, so a build without `-j` runs on one core -- over ten minutes for a
first build that takes a fraction of that in parallel. Nothing in the build depends on serial order.

The first build is slow: it compiles bgfx, bx, bimg and shaderc from `external/bgfx.cmake`.

Build just the library:
```bash
cmake --build build --target projectV -j$(nproc)
```

Examples land in `build/examples/<name>/`, with their renderer folders and scenes staged beside
the binary. Run them from there -- the engine resolves relative paths against the working
directory, and renderer folders name their shaders relative to it in `resources.json`:

```bash
# Feature examples (the basic renderer, unless rendering is the subject)
cd build/examples/hello_voxel       && ./hello_voxel
cd build/examples/scene_previewer   && ./scene_previewer scenes/SmallVox
cd build/examples/entities          && ./entities
cd build/examples/sandbox           && ./sandbox            # SANDBOX_SELFTEST=600 plays itself
cd build/examples/physics           && ./physics            # DEMOS_SELFTEST=1 checks all eleven (DEMOS_ONLY=9 or 2,11: some)
cd build/examples/world_loading     && ./world_loading      # --headless-check: the loader alone, no window
cd build/examples/renderer_gallery  && ./renderer_gallery --renderer fast
cd build/examples/advanced_renderer && ./advanced_renderer
cd build/examples/terrain_generator && ./terrain_generator
cd build/examples/scene_editor      && ./scene_editor
cd build/examples/mp_server         && ./mp_server          # then ./mp_client in build/examples/mp_client (selftest.sh: all headless)
# Demos (_demo: many features at once, the full renderers)
cd build/examples/voxel_mayhem_demo && ./voxel_mayhem_demo  # MAYHEM_SELFTEST=1 checks it; MAYHEM_BENCH=cannon times a break
cd build/examples/explosion_demo    && ./explosion_demo --map city|house|forest --graphics 1|2|3
cd build/examples/party_demo        && ./party_demo         # hosts a party with bots; --host joins one (party_demo_server: dedicated; selftest.sh)
cd build/examples/playground_demo   && ./playground_demo    # a sandbox to break things in together; --host joins one (playground_demo_server; selftest.sh)
```

Examples live in `examples/`, numbered in reading order (`00-hello-voxel` .. `95-playground-demo`).
Two kinds: **feature examples** (`00`-`70`) teach one part of the engine and draw with
`examples/shared/basicRenderer` (one pass: albedo under a fixed sun; `shared/basic_renderer.h`), unless
rendering is what they teach (`30-renderers`, `40-advanced-renderer`, the scene editor's viewport).
**Demos** (`80`+, `_demo` suffix on the folder and the target) combine many features to show them off, and
draw with the full renderers (`shared/voxelRenderer`, the explosion's `nightRenderer`; Voxel Mayhem
draws with the gallery's restir-pt, its uniforms in `shared/restir_pt.h`, `MAYHEM_RENDERER=voxel` the voxel renderer).
Target names are not the directory names: `00-hello-voxel` builds `hello_voxel`, `30-renderers`
builds `renderer_gallery`.

Unit tests (CPU only, no display) live in `tests/unit/` and build in the `dev` preset:

```bash
ctest --preset dev
```

Real sockets on localhost are left out too: `ctest --preset net` (ENet, and the multiplayer example as
three headless processes, and a hundred bot clients against a server: `scale.sh`). `--preset clang` builds the
tests with Clang, to check the golden physics recording on a second compiler.

The physics soak (2000 bodies, ten simulated minutes, ~30 s) is left out of that run; run it
after changing physics:

```bash
ctest --preset soak
```

Manual test harnesses live in `tests/manual/` and are opt-in:

```bash
cmake --preset dev -DPROJV_BUILD_MANUAL_TESTS=ON
```

Presets: `dev` (default), `release` (`PROJV_LOG_MINIMAL=ON`), `vcpkg` (every dependency through
`find_package`, so the install carries only ProjectV rather than the vendored copies too).

Options: `PROJV_PHYSICS_ASSERTS` (Jolt's asserts; off in `release`), `PROJV_BUILD_EXAMPLES`, `PROJV_BUILD_TESTS`, `PROJV_BUILD_MANUAL_TESTS`, `PROJV_USE_X11`, `PROJV_LOG_MINIMAL`,
and the seven `PROJV_LOG_<CATEGORY>` switches. They are `PUBLIC` on the target, so consumers
inherit them.

Full build documentation, including platform notes and troubleshooting: `docs/BUILDING.md`.

`20-mesh-voxelizer` and `60-scene-editor` need their submodules; without them the configure step
skips each with a message naming what to check out.

```bash
git submodule update --init --recursive
```

Manual tests (need a display):
```bash
cd tests/manual && make && ./exit_path a && ./exit_path b
```

## Codebase Conventions

- Namespace `projv` for core types, `projv::utils` for utilities, `projv::graphics` for GPU,
  `projv::runtime` for the Scene bridge.
- `projv::Application` (`core/application.h`) owns the loop; add systems with `addSystem(Stage, name, fn)`.
  `World` is an `entt::registry` (`core/world.h`); global resources live in `world.ctx()`.
- **Layering:** `utils/` and `data_structures/` never include EnTT, `core/world.h`/`application.h`/
  `events.h` or `runtime/`. The `layering` ctest enforces it. `runtime/` is the one layer that sees both.
- Component deletion is `utils::deleteComponent`. Freed component and chunk rows are **reused**
  (`Scene::slots`), so a bare `ComponentHandle` is only good within a frame: anything held across
  frames keeps a `ComponentRef` and checks it with `utils::resolve`. `utils::isComponentAlive` is the
  liveness check for a handle. The scene editor turns recycling off (it still keeps bare handles).
- Create rows through the engine (`addComponent`, `reserveComponentSlot`/`placeComponent`,
  `allocateChunkSlot`), not `push_back`, so freed rows are reused; the top-level components are
  `utils::rootComponents`.
- **Physics is Jolt, and only `src/runtime/physics/` includes it** (the `layering` ctest enforces
  it). Those files build as the `projv_physics` object library with Jolt's SIMD flags and defines;
  no public header names a Jolt type. `runtime::PhysicsWorld` (`runtime/physics/physics_world.h`)
  is the core. It must stay deterministic and restorable: no clocks, thread counts or unordered
  iteration in anything that feeds a step, and a Jolt upgrade is deliberate, with the physics
  tests run. Plan: `docs/plans/physics-jolt.md`. The submodule carries one patch, applied at configure
  time (`cmake/patches/jolt-applied-impulses.patch`: the solver's contact impulses, for impact load
  shares); an upgrade re-makes it. Plan: `docs/plans/impact-loads.md`.
- **The golden recording** (`tests/unit/data/physics_golden.rec`, `test_physics_golden.cpp`) is one
  committed session every build must replay bit for bit (twice: `physics_golden_tiles.rec` is the same
  session with static voxels as tile tables). A change to what the simulation does --
  a Jolt upgrade, a new rule in the step, a new log field -- fails it on purpose: find out why, and
  when the change is meant, regenerate it with `PROJV_WRITE_GOLDEN=1 build/bin/projv_unit_tests
  -tc="golden*"` and commit the file with the change.
- **No world rollback** (plan, D9): `savePhysics` does not rewind across anything that makes or
  removes bodies, by design. Don't build toward it without asking.
- **Networking** (plan: `docs/plans/networking.md`). `net/` (transport, wire format) follows the
  utils rule: no EnTT, no runtime. Only `src/net/transport_enet.cpp` includes ENet (`layering`).
  Tests use `net::LoopbackNetwork` (one process, a clock the test owns); real sockets only in
  `ctest --preset net`. Anything read off the wire is checked (`BitReader::failed`), never asserted.
- **What crosses machines is a `NetId`** (`runtime/net.h`), never an Entity, handle, ref or body
  id. Physics makes a step's new bodies in NetId order and applies commands in
  (tick, issuer, sequence) order, so nothing that feeds a step may depend on registry or arrival
  order. `PhysicsSettings::record` + `PhysicsWorld::replay` check a whole session for determinism.
- **Physics input is checked, never asserted on.** Jolt asserts (and so stops a dev build) on input
  it cannot take -- a velocity over a body's limit, a degenerate shape. Everything that reaches Jolt
  goes through `PhysicsWorld`, which refuses, clamps or logs first; keep it that way when adding calls.
- **Where data goes.** Engine-read data: a `ComponentRecord` field. Data *about the voxel structure*
  written by tools (editor boolean ops, primitive parameters): an attachment (`utils/attachments.h`),
  saved in compose.json. What a running program *does* (physics, spawners, gameplay): ECS components
  on authored entities, saved as themselves in `entities.json` beside compose.json
  (`runtime/entities.h`, `docs/data_structures/entities_data_structure.md`).
- Components have a persistent `localId` (the entry's `"id"`), unique among siblings. Entity files
  link by it; never persist a `ComponentHandle`.
- Chunk handle = index into `Scene.chunks` (stable).
- Component handle = index into `Scene.components` (stable).
- Geometry pool blobs are refcounted; `chunk.geometryPoolIndex < 0` = unpooled.
- `chunkQueue` on Chunk is the legacy edit staging area (to be removed after P1 verification).
- `editQueue` on ComponentRecord is the new per-component edit queue (P1+).
- `forkBlob` creates a COW copy without decrementing the original's refCount.
- A blob's voxels are a **tile table** (`utils/tile_table.h`): dense region and block masks, tiles for
  solid one-material children, leaves only for partly filled bricks. It is what the GPU marches, what
  edits change in place (`applyTileEdits`) and what physics reads, and `.data` v4 stores it. **Read and
  write a blob's voxels through `utils/blob_voxels.h`** (`blobTiles`, `setBlobTiles`, `editBlobTiles`,
  and `blobTree`/`setBlobTree` for code that still takes a tree): a blob holds whichever form was
  written last and derives the other on demand; tiles derived from a tree replace it. Code that makes
  voxels off the main thread (a streaming worker) builds the table there and installs it with
  `setBlobTiles`/`replaceChunkTiles`. Plan: `docs/plans/voxel-storage-tiles.md`.
- A blob's `contentStamp` names its voxel content (physics shapes are cached by it). Code that
  replaces a blob's voxels in place calls `markBlobContentChanged(blob)` (the blob_voxels setters do), never just clears
  `contentBoundsValid`. What voxels collide as is set by `CollisionParams::representation`
  (`utils/collision_geometry.h`): `Boxes` (the default) is `utils::buildCollisionPieces`, and nothing
  else; `Tiles` makes voxel bodies collide as their blob's tile table, read in place
  (`src/runtime/physics/voxel_shape.h`, `utils/tile_collision.h`) -- static ones as they are, moving
  ones weighed and merged by block once, colliding with each other box by box. Voxel Mayhem's maps use
  Tiles; `PROJV_COLLISION=tiles` runs anything (the unit suite, a demo) on it.
  Plan: `docs/plans/voxel-collision-shape.md`.
- **Cheap debris** (`PhysicsConfig::debris`, off by default): a break's pieces of up to `maxVoxels`
  are `Debris`, not bodies -- drawn, falling and bouncing against static bodies, never in the solver
  or a query. Voxel Mayhem's maps use it (64 voxels). Plan: `docs/plans/cheap-debris.md`.

## Where things are

| Path | What |
|---|---|
| `include/` `src/` | The engine. `core/`, `graphics/`, `utils/`, `data_structures/`, `runtime/`. |
| `cmake/` | `ProjectVConfig.cmake.in`, plus the `projv_compile_shaders()` and `projv_add_example()` helpers. |
| `examples/` | Twelve feature examples and four `_demo`s, numbered in reading order; `shared/` holds their renderers. See `examples/README.md`. |
| `tests/unit/` | The automated suite (doctest, ctest). CPU only. |
| `tests/manual/` | Harnesses that need a display. Not examples. See `tests/README.md`. |
| `docs/data_structures/` | Format references: the `.data` container, compose scenes, entities, tree64, renderers. |
| `docs/plans/` | Design notes, delivery logs, and `known-latent-issues.md` / `test-suite-notes.md`. Plans live here, in the repo -- never outside it -- and are kept current as work lands. |

Two documents worth reading before changing engine behaviour:

- `docs/plans/known-latent-issues.md` — real defects that are not currently triggered, and the
  bundled assets that no longer load.
- `docs/plans/test-suite-notes.md` — what a test suite should cover, with the confirmed failure
  mode for each entry.

<!-- headroom:rtk-instructions -->
# RTK (Rust Token Killer) - Token-Optimized Commands

When running shell commands, **always prefix with `rtk`**. This reduces context
usage by 60-90% with zero behavior change. If rtk has no filter for a command,
it passes through unchanged — so it is always safe to use.

## Key Commands
```bash
# Git (59-80% savings)
rtk git status          rtk git diff            rtk git log

# Files & Search (60-75% savings)
rtk ls <path>           rtk read <file>         rtk grep <pattern>
rtk find <pattern>      rtk diff <file>

# Test (90-99% savings) — shows failures only
rtk pytest tests/       rtk cargo test          rtk test <cmd>

# Build & Lint (80-90% savings) — shows errors only
rtk tsc                 rtk lint                rtk cargo build
rtk prettier --check    rtk mypy                rtk ruff check

# Analysis (70-90% savings)
rtk err <cmd>           rtk log <file>          rtk json <file>
rtk summary <cmd>       rtk deps                rtk env

# GitHub (26-87% savings)
rtk gh pr view <n>      rtk gh run list         rtk gh issue list

# Infrastructure (85% savings)
rtk docker ps           rtk kubectl get         rtk docker logs <c>

# Package managers (70-90% savings)
rtk pip list            rtk pnpm install        rtk npm run <script>
```

## Rules
- In command chains, prefix each segment: `rtk git add . && rtk git commit -m "msg"`
- For debugging, use raw command without rtk prefix
- `rtk proxy <cmd>` runs command without filtering but tracks usage
<!-- /headroom:rtk-instructions -->

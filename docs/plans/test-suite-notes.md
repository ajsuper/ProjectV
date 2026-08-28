# Notes toward a ProjectV test suite

Running notes on what an official test suite should cover, collected while doing the
packaging/tooling work. This is a design input, not a plan of record.

Each entry says what to test and, where known, **how it was caught** — a bug that was
found by hand is a test that did not exist.

---

## Ground rules learned the hard way

**Test binaries must depend on the engine archives, not just their own source.**
`tests/manual/Makefile` originally read `exit_path: exit_path.cpp`. When the engine was
rebuilt with a fix deliberately reverted, `make` reported the test up to date, did not
relink, and the test **passed against the old binary** — a false PASS during a regression
check. Any build rule for a test must list the libraries it links.

**A test that sets the value it is checking proves nothing.** The first draft of the
exit-path test assigned `RenderInstance::shouldClose` itself and then asserted it was
true. It passed with the engine's own assignment deleted. Tests must drive the real entry
point (`renderConstructedRenderer`) and only ever *observe* engine state.

**Every fix needs its revert run.** Both engine fixes below were confirmed by removing the
fix, rebuilding, and watching the test fail with the exact original symptom. Without that
step there is no evidence the test is attached to the behaviour.

**"It runs" is not "it works."** Six path-tracer renderers were confirmed to start, submit frames
and exit cleanly — while loading *zero chunks*, because their scene is a `.data` version the
loader rejects (see `known-latent-issues.md` issue 5). Any test that launches an example must
assert on something the example produced: the chunk count from the load, a frame time, a pixel.
A clean exit proves nothing.

---

## 1. Application lifecycle — partly covered by `tests/manual/exit_path.cpp`

- **No Shutdown stage registered → clean exit.** `createApp()` left `Application::Shutdown`
  empty while `runApplication()` called it unconditionally.
  *Confirmed failure mode:* `terminate called after throwing an instance of
  'std::bad_function_call'`, exit 134.
- **A registered Shutdown stage actually runs**, exactly once, after the loop ends.
- **`RenderInstance::shouldClose` tracks the window manager.** False before any request,
  true within one frame of one, observed only through `renderConstructedRenderer`.
  *Confirmed failure mode:* loop never ends.
- **Not yet covered:** `closeAppFlag` set during Startup (should the loop body run at all?);
  Shutdown running when the loop is ended by something other than a close request;
  `frameCount` monotonicity.
- **Blocked:** moving/copying an `Application` — `assignSystemStage` captures `[=, &app]`,
  so this is known-broken. See `known-latent-issues.md`; write the test with the fix.

## 2. Paths — partly covered by `tests/manual/paths_probe.cpp`

`executableDirectory()` must return the same real directory for all five invocations:
relative path, absolute path, from a foreign CWD, through a symlink, and by bare name off
`PATH`. All five verified. Worth keeping as a test because four of the five are one
platform `#ifdef` away from breaking.

## 3. Renderer specification loading — **the biggest current gap**

`resources.json` / `render.json` are data, and there is nothing checking that the data in
the tree still matches the loader.

- **Every renderer folder in the repo must load.** Nine of twelve `resources.json` files
  were still using the `resizable` key that the engine replaced with `sizeMode` — which
  means **ScenePreviewer, all seven PathTracer renderers, and terrain_generator's
  worldCascade renderer could not start at all.** Caught by running one of them, not by
  any test. A trivial "load every `*/resources.json` under `examples/` and assert no
  throw" test would have caught all nine the day the key changed.
- **Schema rejections should be tested for their message, not just the throw.** The
  loader's `resizable` diagnostic is genuinely good — it names the texture and explains the
  migration. That quality is worth locking in.
- Framebuffer attachments must share `sizeMode` and `scale`; a mismatch must be rejected.
- `sizeMode: "fixed"` with no resX/resY must be rejected.

## 4. Example link lists — gap

`compose_io` gained a dependency on `scene_query` (`addComponent`,
`setComponentTransform`). MeshVoxelizer's CMakeLists was updated and documents it;
**ScenePreviewer's and PathTracer's Makefiles were not, so both failed to link.** Caught
by trying to build them.

**Resolved by the single-target build.** Every example now links `ProjectV::projectV`, so there
is no per-example library list left to go stale. "Every example builds" is still worth a CI job,
but it no longer guards against this specific failure.

## 5. Shader compilation — gap

The twelve `comp*.sh` scripts are gone, replaced by `projv_compile_shaders()`. What needs
testing is now the CMake rule: that every shader in the tree still compiles, and that the
engine's shader library (`pjv_utils_DDA.sc`) and each example's own `sharedShaders/` are both
on the include path.

Worth noting what the scripts' removal settled: the ScenePreviewer README claimed PathTracer's
scripts were missing `-i $PROJECTV_DIR/include`. All seven had it and all seven compiled clean,
so the claim had gone stale without anyone noticing. A test would have kept it honest.

Note the committed `.bin` files can mask a broken shader source: delete them before
compiling, or the test only proves the old artefacts still exist.

**Every renderer folder on disk should be reachable.** PathTracer ships seven renderer
folders; its menu offers six. `radianceCascadeRenderer/` is complete — its shaders compile
and its `resources.json` is valid — but nothing can select it. A test asserting that every
`*/render.json` under an example is reachable from that example's own entry points would
have caught it. The `--renderer <name>` rehost removes the failure mode by making the
folder the unit of selection.

## 6. Scene / Compose I/O — gap, and the highest-value pure-CPU target

Needs no window, so it is the cheapest thing to run in CI:

- Round-trip: `saveComposeToDisk` → `loadComposeFromDisk` preserves chunk count, component
  count, transforms, and palettes.
- `.data` container versions: v2 and v3 load; v1 is rejected with a clear message. Several
  bundled scenes are still v1 and load as **zero chunks** — currently documented in prose
  rather than asserted anywhere.
- Every bundled scene under `examples/` loads and reports a plausible bounding box. A scene
  that silently loads as zero chunks is the failure this catches.
- Palette: `internMaterial` at the 255-entry boundary; `removeMaterial` and reindexing.

## 7. Voxel math — gap, trivially testable

`floorDiv`/`floorMod` across negative coordinates (the bug they were introduced to fix),
Z-order round-trips (`calculateZOrderIndex` ↔ `inverseZOrderIndex`), header position
packing. Pure functions, no fixtures — these should be the first unit tests written.

## 8. Editing — gap

`queueVoxelAdd`/`queueVoxelRemove` → `updateScene` on both Chunk and Grid components;
grid expansion into negative coordinates with `originCellCoord` rebasing; COW fork
semantics and `refCount`. Much of this already exists as assertions inside
`docs/examples/editing_p1/main.cpp`, which is a test driver wearing an example's clothes —
it should become the seed of the real suite rather than being deleted.

## 9. Packaging — gap, needed by the current work

- Install, then build a scratch consumer against `find_package(ProjectV CONFIG REQUIRED)`
  with no `-I`/`-L` of its own.
- The install tree's `include/` contains only `projv/`.
- `projectv new <template>` output builds and runs from a directory unrelated to the engine.
- **Install is refused for the right reason.** A submodule build disables install/export
  deliberately, because exporting vendored glm/spdlog would collide with the consumer's. A test
  should assert that the `vcpkg` preset installs and the `dev` preset says why it will not,
  rather than either silently producing a broken install tree.
- Each example's assets are staged beside its binary and it runs from that directory.
- **The package's dependency surface does not grow silently.** Consolidating the build initially
  linked `spdlog::spdlog` and `glm::glm` rather than their header-only variants. spdlog's compiled
  target puts `SPDLOG_COMPILED_LIB` on its PUBLIC interface, which quietly made `libspdlog` a hard
  link requirement of every consumer — caught only because a hand-written test Makefile started
  failing with `undefined reference to spdlog::default_logger_raw()`. A test that links the
  installed package from outside CMake would catch this class of change directly.

---

## Suggested shape

Two tiers, because the split is what decides whether it runs in CI:

- **Headless** (`tests/unit`) — voxel math, compose I/O, renderer-spec *parsing*, palette,
  editing. No window, no GPU, runs anywhere. This is where most of the value is and it is
  the part CI can actually gate on.
- **Windowed** (`tests/manual`) — lifecycle, real renderer construction, shader
  compilation, the examples themselves. Needs a display and a GPU; currently exercised
  through `distrobox enter projectv-dev`.

No framework has been chosen. Given the engine vendors nothing for testing today, a single
header (doctest or Catch2) added as a submodule is the least intrusive option, and the
headless tier is small enough that even a plain `assert`-and-exit-code harness would do to
start.

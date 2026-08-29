# Agent Notes

## Build Commands

Everything builds from the top-level CMake project. There are no per-example Makefiles and no
per-example shader scripts any more -- one target, one shader rule.

```bash
cmake --preset dev            # configure (submodule deps, examples on)
cmake --build --preset dev    # build the library, the examples, and their shaders
```

The first build is slow: it compiles bgfx, bx, bimg and shaderc from `external/bgfx.cmake`.

Build just the library:
```bash
cmake --build build --target projectV
```

Examples land in `build/examples/<name>/`, with their renderer folders and scenes staged beside
the binary. Run them from there -- the engine resolves relative paths against the working
directory, and renderer folders name their shaders relative to it in `resources.json`:

```bash
cd build/examples/hello_voxel      && ./hello_voxel
cd build/examples/scene_previewer  && ./scene_previewer scenes/SmallVox
cd build/examples/renderer_gallery && ./renderer_gallery --renderer world-cascade
cd build/examples/scene_editor     && ./scene_editor
```

Examples live in `examples/`, numbered in reading order (`00-hello-voxel` .. `60-scene-editor`).
Target names are not the directory names: `00-hello-voxel` builds `hello_voxel`, `30-renderers`
builds `renderer_gallery`.

Manual test harnesses live in `tests/manual/` and are opt-in:

```bash
cmake --preset dev -DPROJV_BUILD_MANUAL_TESTS=ON
```

Presets: `dev` (default), `release` (`PROJV_LOG_MINIMAL=ON`), `vcpkg` (every dependency through
`find_package`, so the install carries only ProjectV rather than the vendored copies too).

Options: `PROJV_BUILD_EXAMPLES`, `PROJV_BUILD_MANUAL_TESTS`, `PROJV_USE_X11`, `PROJV_LOG_MINIMAL`,
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

- Namespace `projv` for core types, `projv::utils` for utilities, `projv::graphics` for GPU.
- Chunk handle = index into `Scene.chunks` (stable).
- Component handle = index into `Scene.components` (stable).
- Geometry pool blobs are refcounted; `chunk.geometryPoolIndex < 0` = unpooled.
- `chunkQueue` on Chunk is the legacy edit staging area (to be removed after P1 verification).
- `editQueue` on ComponentRecord is the new per-component edit queue (P1+).
- `forkBlob` creates a COW copy without decrementing the original's refCount.

## Where things are

| Path | What |
|---|---|
| `include/` `src/` | The engine. `core/`, `graphics/`, `utils/`, `data_structures/`. |
| `cmake/` | `ProjectVConfig.cmake.in`, plus the `projv_compile_shaders()` and `projv_add_example()` helpers. |
| `examples/` | Seven examples, numbered in reading order. See `examples/README.md`. |
| `tests/manual/` | Harnesses that need a display. Not examples. See `tests/README.md`. |
| `docs/data_structures/` | Format references: the `.data` container, compose scenes, tree64, renderers. |
| `docs/plans/` | Design notes, delivery logs, and `known-latent-issues.md` / `test-suite-notes.md`. |

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

# ProjectV Examples

Seven programs, ordered so that reading them in sequence teaches the engine. Each one is a
complete application, not a snippet, and each has its own README explaining not just how to run it
but why it exists and what it decided differently from its neighbours.

Everything builds from the repository root:

```bash
cmake --preset dev
cmake --build --preset dev
```

Binaries land in `build/examples/<name>/` with their renderer folders and scenes staged beside
them. **Run each from its own directory** — the engine resolves relative paths against the working
directory, and a renderer folder names its shaders relative to it in `resources.json`.

```bash
cd build/examples/hello_voxel && ./hello_voxel
```

## The examples

| | Example | What it teaches | Needs |
|---|---|---|---|
| **00** | [hello-voxel](00-hello-voxel/) | The startup sequence, at the smallest size that draws anything. Builds its voxels in memory, so there is nothing to download. | — |
| **10** | [scene-previewer](10-scene-previewer/) | Loading a scene from disk, framing it automatically, flying around it. Pure albedo, no lighting — it shows what is *in* a scene rather than how it looks. | — |
| **20** | [mesh-voxelizer](20-mesh-voxelizer/) | Getting your own data in: meshes and Minecraft worlds to Compose scenes. Headless command-line tool. | Assimp submodule |
| **30** | [renderers](30-renderers/) | Seven renderers over one scene and one camera — the comparison is the point. Accumulation, reprojection, per-face GI, and four cascade variants. | — |
| **40** | [advanced-renderer](40-advanced-renderer/) | The good renderer, brought up to the engine as it is today: fourteen passes, world-space probe GI, animation, transparency and refraction. | — |
| **50** | [terrain-generator](50-terrain-generator/) | Procedural generation and streaming: chunks built on worker threads and uploaded while you fly. | — |
| **60** | [scene-editor](60-scene-editor/) | A complete tool. ImGui docking, the scene living inside a panel, tools, undo, and Lua-scripted brushes. | ImGui + Lua submodules |

The numbering is reading order, not priority. Two examples carry third-party submodules of their
own; without them the configure step skips that example with a message naming what to check out,
rather than failing the build. `git submodule update --init --recursive` gets everything.

## Where to start

**Never used the engine before** — read [00-hello-voxel](00-hello-voxel/) end to end. It is one
file, it builds its own geometry, and it is the only place the whole startup sequence appears
without anything else competing for attention.

**Want to see your own model** — [20-mesh-voxelizer](20-mesh-voxelizer/) converts it, then
[10-scene-previewer](10-scene-previewer/) shows you what came out. That pairing is deliberate: the
previewer applies no lighting at all, so anything that looks wrong there is wrong in the data.

**Here about rendering** — [30-renderers](30-renderers/) is the survey and
[40-advanced-renderer](40-advanced-renderer/) is the destination.

**Building a tool** — [60-scene-editor](60-scene-editor/) is the one that renders the scene into a
panel rather than the window, which is the thing every editor-shaped application needs first.

## A caveat about the bundled scenes

Several bundled `.data` files are container version 1, which the current loader rejects. A scene
made only of those **parses but contains no geometry** — the example starts, renders an empty
frame, and exits cleanly. The renderer gallery says so at startup; the affected assets are listed
in [docs/plans/known-latent-issues.md](../docs/plans/known-latent-issues.md). If an example looks
empty, check the `Loaded N chunk(s)` line before assuming it is broken.

## Not examples

`tests/manual/` holds two programs that used to live here: `editing_p1`, a CPU-only driver with 86
self-checks over the editing API, and `edit_demo`, an interactive editing harness. They exercise
the engine rather than demonstrate it, and they are built only with
`-DPROJV_BUILD_MANUAL_TESTS=ON`.

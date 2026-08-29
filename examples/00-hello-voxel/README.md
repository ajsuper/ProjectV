# 00 — Hello Voxel

The smallest program that opens a window and draws voxels. Under 310 lines including its comments,
one fragment shader, and **no scene file**: the geometry is built in memory at startup, so it runs
the moment it is compiled with nothing to download and nothing to voxelize first.

## Why

Every other example starts somewhere past the beginning. The previewer already has automatic scene
framing, a frame profiler, cursor capture and scroll-driven movement speed before it draws a pixel;
the renderer gallery has seven renderers to choose between. Those are all reasonable things for
those programs to have, and all of them are noise if the question you actually have is *what is the
minimum?*

This is the answer to that question, and it is the file to copy when starting something new.

## How to build

```bash
cmake --preset dev
cmake --build --preset dev --target hello_voxel
```

## How to run

```bash
cd build/examples/hello_voxel
./hello_voxel
```

A white sphere with red, green and blue bars marking +X, +Y and +Z.

| Input | Action |
|-------|--------|
| `W` / `S` | Move forward / backward |
| `A` / `D` | Strafe left / right |
| `R` / `F` | Move up / down |
| Mouse | Look around (cursor is captured; `Esc` releases it, left-click re-captures) |

Close the window to quit. That works, which is worth stating because it is a thing this example
demonstrates deliberately — see below.

## The startup sequence

This is the part worth reading. Every windowed ProjectV application does these seven things, and
the rest of the examples are this plus their own subject.

```
1. RenderInstance::initialize          create the window, bring bgfx up
2. createGlobalResource<Scene/GPUData> put the scene and its GPU mirror in the world
3. buildScene() or loadComposeFromDisk get some voxels
4. loadRendererSpecification           read render.json + resources.json
5. loadShader                          load the compiled vertex shader
6. constructRendererSpecification      turn the description into GPU objects
7. createTexturesForScene              upload the voxels
```

Then each frame: set the uniforms the shaders read, and call `renderConstructedRenderer`.

The application itself is three (here, four) functions registered against `SystemStage::Startup`,
`Update`, `Render` and `Shutdown`, and `runApplication` drives the loop.

## Building geometry in memory

Most examples load a scene from disk. This one writes one, which is the shorter path to
understanding what a scene *is*:

- A **component** owns geometry and a material palette. Palette slots are per-component, and the
  slot number is what the voxel data stores.
- A **chunk** is one voxel volume. This one is "loose" — placed directly in the world, as opposed
  to being a cell in a grid.
- Geometry is authored through a **brick map**, a plain 3D array of material slots that is easy to
  write into. `updateChunkFromBrickMap` compresses it into the tree64 the GPU traverses, so nothing
  in this file has to know that format.
- Geometry then moves into a refcounted **pool blob** (`internChunkGeometry`) so several chunks can
  share one volume.

Colours come from `internMaterial`, which dedupes: you ask for a colour and get back the slot that
has it, rather than managing indices yourself.

## Two things no other example demonstrates

**It exits cleanly.** The engine records a window-manager close request on
`RenderInstance::shouldClose` and does nothing else with it. The application decides what a close
request means:

```cpp
if (renderInstance.shouldClose) {
    app.closeAppFlag = true;
}
```

That indirection is deliberate. A tool with unsaved work wants to raise a prompt instead, which it
could not do if the engine closed the window on its behalf.

**It finds its assets from its own location.** The engine's load functions resolve a relative path
against the working directory, which means an application launched from elsewhere would not find
its renderer folder. `projv::core::executableDirectory()` is the one piece of information an
application cannot compute for itself; everything after it is ordinary path composition:

```cpp
const auto assets = projv::core::executableDirectory() / "helloRenderer";
```

There is no global "asset root" in the engine and nothing to configure — several roots are just
several paths.

## The renderer

One pass, straight to the back buffer. `helloRenderer/render.json` is the pass order — a single
entry with `frameBufferOutputID: -1`, meaning the window. `helloRenderer/resources.json` names the
shader and declares the three uniforms it reads. There are no render targets and no framebuffers,
which is what makes this the smallest renderer the engine can be given.

`helloShaders/hello.frag` is the whole thing: build a camera ray with `rayStartDirection`, march it
with `raySceneIntersect`, and write `fetchVoxelColor`. Both calls come from `pjv_utils_DDA.sc`, the
engine's shader library, which the build puts on shaderc's include path automatically.

The one liberty it takes is a fixed tint per face, so the six faces of a voxel are
distinguishable. That is not a light — it is a constant per axis. Without it every face is the same
colour and the sphere reads as a flat disc. A real renderer traces a shadow ray there instead; see
[40-advanced-renderer](../40-advanced-renderer/).

## ProjectV features used

| Feature | Usage |
|---------|-------|
| **Core ECS** | `Application`, global resources, all four system stages |
| **Core paths** | `executableDirectory` — assets relative to the binary, not the CWD |
| **Core math** | `vec2`/`vec3`/`quat` |
| **Voxel management** | `createVoxelBrickMap`, `brickMapSetVoxel`, `updateChunkFromBrickMap`, `internChunkGeometry` |
| **Material** | `internMaterial`, `packColor` |
| **Disk I/O** | `loadRendererSpecification`, `loadShader` |
| **Manage resources** | `constructRendererSpecification` |
| **GPU interface** | `createTexturesForScene`, `setUniformToValue`, `destroyGPUData` |
| **Render instance** | Window creation, active renderer, `shouldClose` |
| **Perform renderer** | `renderConstructedRenderer` |

## Third-party dependencies

None beyond the engine's own. No image loader, no blue-noise LUT, no ImGui — there is nothing
stochastic here and no interface to draw.

## Where to go next

[10-scene-previewer](../10-scene-previewer/) is this program plus a real scene loaded from disk,
automatic camera framing, and a second and third render pass. The diff between the two is a good
second thing to read.

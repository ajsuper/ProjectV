# PathTracer

An interactive real-time path tracer that loads a pre-voxelized scene from disk and renders it using ProjectV's tree64-based path tracing pipeline. The camera can be freely navigated, and the renderer accumulates samples across frames when the camera is still to progressively refine the image.

The included scene is the **Sponza atrium** (`SponzaScene/`), voxelized with [ObjVoxelizer](../ObjVoxelizer/).

## Overview

The renderer is built around a four-pass pipeline defined in `tree64Renderer/render.json`:

1. **path_trace** — casts rays into the tree64 voxel structure and accumulates radiance samples
2. **accumulate** — blends the current frame with the history buffer when the camera is still
3. **denoise_ao_gi** — applies a denoising pass over ambient occlusion and global illumination
4. **post_and_display** — tone-maps and outputs the final image to the screen

A blue-noise LUT (`LDR_RGBA_7.png`) is uploaded to the GPU at startup to decorrelate samples between frames.

## How to Build

```bash
cd PathTracer
make
```

Requires ProjectV to be built at `../../../` with its libraries and bgfx installed.

## How to Use

```bash
./main.o
```

The application opens a 2560×1440 window and begins rendering the Sponza scene immediately. Navigate the scene with the keyboard:

| Key | Action |
|-----|--------|
| `W` / `S` | Move forward / backward |
| `A` / `D` | Strafe left / right |
| `R` / `F` | Move up / down |
| `Q` / `E` | Rotate camera left / right |

When the camera is stationary, the renderer accumulates samples each frame, progressively reducing noise. Any movement resets accumulation.

To render a different scene, replace `SponzaScene/` with any directory produced by ObjVoxelizer and update the `loadSceneFromDisk` path in `main.cpp`.

## ProjectV Features Used

| Feature | Usage |
|---------|-------|
| **Core ECS** | `Application`, `world`, global resources, system stage assignment (`Startup`/`Update`/`Render`) |
| **Core Math** | `vec2`/`vec3`/`vec4`, `cos`/`sin` — camera direction and uniform packing |
| **Logging** | `info`/`warn` via the spdlog wrapper for structured output and frame profiling |
| **Voxel I/O** | `loadSceneFromDisk` — deserializes the chunked voxel scene from disk |
| **GPU Interface** | `createTexturesForScene`, `GPUData` — uploads voxel scene data to the GPU |
| **Manage Resources** | `RendererSpecification`, `ConstructedRenderer`, `setTextureToData` — builds the renderer pipeline |
| **Disk I/O** | `loadRendererSpecification`, `loadShader` — loads renderer config and compiled shaders |
| **Render Instance** | Window creation, active renderer management, window resolution query |
| **Perform Renderer** | `renderConstructedRenderer`, `setUniformToValue` — drives per-frame rendering and uniform upload |

## Third-Party Dependencies

- [stb_image](https://github.com/nothings/stb) — blue-noise texture loading
- [bgfx](https://github.com/bkaradzic/bgfx) — cross-platform GPU abstraction (via ProjectV)
- [GLFW](https://www.glfw.org/) — window and input handling (via bgfx/ProjectV)

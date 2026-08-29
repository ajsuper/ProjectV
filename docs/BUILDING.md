# Building ProjectV

The short version lives in the [README](../README.md). This is everything else.

## Requirements

| | |
|---|---|
| Compiler | C++17. GCC and Clang are tested. |
| CMake | 3.20 or newer (bgfx.cmake requires it) |
| GPU | Vulkan on Linux, Metal on macOS |
| System packages | **GLFW** is the one hard system dependency. On Linux also `wayland-devel` (or X11 development headers if building with `PROJV_USE_X11=ON`). |

Fedora:

```bash
sudo dnf install gcc-c++ cmake git glfw-devel wayland-devel
```

Everything else — bgfx, bx, bimg, glm, spdlog, nlohmann/json — is a submodule and is built for you.

## First build

```bash
git clone https://github.com/ajsuper/ProjectV.git
cd ProjectV
git submodule update --init --recursive

cmake --preset dev
cmake --build --preset dev
```

**The first build is slow.** It compiles bgfx, bx, bimg and `shaderc` from source; `shaderc` alone
drags in glslang, SPIRV-Tools and glsl-optimizer. Subsequent builds do not repeat it.

## Presets

| Preset | Build dir | What it is for |
|---|---|---|
| `dev` | `build/` | Day-to-day. Submodule dependencies, `RelWithDebInfo`, examples on. |
| `release` | `build-release/` | Optimised, with every optional log category compiled out (`PROJV_LOG_MINIMAL=ON`). |
| `vcpkg` | `build-vcpkg/` | Every dependency through `find_package` from a vcpkg toolchain. Requires `VCPKG_ROOT`. |

```bash
cmake --preset release && cmake --build --preset release
```

## Options

| Option | Default | Effect |
|---|---|---|
| `PROJV_BUILD_EXAMPLES` | `OFF` (`ON` in `dev`) | Build the seven bundled examples |
| `PROJV_BUILD_MANUAL_TESTS` | `OFF` | Build the windowed test harnesses in `tests/manual/` |
| `PROJV_INSTALL` | `ON` | Generate install and export rules |
| `PROJV_USE_X11` | `OFF` | X11 rather than Wayland for GLFW on Linux |
| `PROJV_LOG_MINIMAL` | `OFF` | Compile out every log category except `WARN` and `ERROR` |
| `PROJV_LOG_TRACE` | `ON` | Trace logging |
| `PROJV_LOG_PERF` | `ON` | Performance logging — the frame-time and upload statistics |
| `PROJV_LOG_EDIT` | `ON` | Editing pathway logging |
| `PROJV_LOG_RENDER` | `ON` | Rendering pathway logging |
| `PROJV_LOG_INFO` / `WARN` / `ERROR` | `ON` | The ordinary levels |

The log options are **`PUBLIC` on the target**, so anything linking `ProjectV::projectV` inherits
them. That matters: the macros compile their call sites out entirely, so a consumer built with a
different set than the library gets different behaviour rather than merely different output.

## Running what you built

Binaries land in `build/examples/<name>/` with their renderer folders and scenes staged beside
them. **Run each from its own directory.** The engine's load functions resolve a relative path
against the working directory, and a renderer folder names its shaders relative to it inside
`resources.json`.

```bash
cd build/examples/hello_voxel && ./hello_voxel
```

An application that wants to be launched from anywhere composes its asset paths from
`projv::core::executableDirectory()`; `00-hello-voxel` demonstrates this.

## Shaders

Shaders are compiled as part of the build by `projv_compile_shaders()`
(`cmake/ProjectVShaders.cmake`), which runs bgfx's `shaderc` over a renderer's `vs_*.sc` and
`*.frag` and writes each `.bin` beside its source. The engine's shader library
(`pjv_utils_DDA.sc`) and bgfx's own headers are put on the include path automatically.

```cmake
projv_compile_shaders(TARGET my_app SHADER_DIRS myRenderer/shaders)
```

There are no per-example shader scripts. To invoke `shaderc` by hand:

```bash
build/bin/shaderc -f shader.frag -o shader.bin --type f \
    --platform linux --profile spirv \
    -i external/bgfx/src -i include
```

## Installing

```bash
cmake --install build --prefix /path/to/prefix
```

A consumer then needs nothing but:

```cmake
find_package(ProjectV CONFIG REQUIRED)
target_link_libraries(my_game PRIVATE ProjectV::projectV)
```

Headers install under `<prefix>/include/projv/`, and the exported target points at that directory —
so `#include "core/ecs.h"` keeps working while the install root stays free of a top-level `core/`
and `utils/`.

**A submodule build installs the vendored dependencies too**, so the prefix is self-contained and
`find_dependency` resolves within it. Give it a prefix of its own rather than one that already
carries bgfx or glm from elsewhere. A `vcpkg` build resolves everything through `find_package` and
installs only ProjectV; configure reports which case you are in.

## Platform notes

**Linux** is the primary platform. GLFW uses Wayland by default; `-DPROJV_USE_X11=ON` selects X11.
The renderer backend is Vulkan.

**macOS** works and the renderer backend is Metal, but it is not the development platform and gets
less exercise. bgfx.cmake handles the toolchain; the Homebrew GLFW path that used to be hardcoded
in the build is gone.

**Windows is not supported.** Not "untested" — `src/graphics/render_instance.cpp` never selects a
renderer backend for it, so `bgfx::init` runs with an unset type. The dependencies are all
cross-platform and the build system no longer stands in the way, but the engine will not run until
that is fixed. It is deliberately not claimed in CI.

## Troubleshooting

**`bgfx was not found and external/bgfx.cmake is missing`** — the submodules are not checked out.
`git submodule update --init --recursive`.

**Configure says it is skipping `20-mesh-voxelizer` or `60-scene-editor`** — those carry submodules
of their own (Assimp; ImGui and Lua). Same command. This is a message, not an error: the rest of
the build proceeds.

**A CMake error naming sources under `external/bimg/3rdparty` or `l-smash`** — `external/bgfx.cmake`
has drifted from the `bgfx`/`bx`/`bimg` pins. Its build files reference sources that move between
bgfx revisions, so the four must be checked out together. `git submodule update --init --recursive`
restores the recorded pins.

**An example renders nothing** — check the `Loaded N chunk(s)` line. Several bundled scenes are
`.data` version 1, which the current loader rejects; the scene then parses but holds no geometry.
See [plans/known-latent-issues.md](plans/known-latent-issues.md).

**Shaders do not rebuild after an edit** — `projv_compile_shaders` depends on the `.sc`/`.frag`
sources and every shared `.sc` in the same directory. A shared include living somewhere else will
not trigger a rebuild; delete the `.bin` files to force one.

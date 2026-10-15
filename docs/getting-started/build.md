# Build

Requirements, presets, options, installing, troubleshooting.

## Requirements

| | |
|---|---|
| Compiler | C++20. GCC and Clang are tested. |
| CMake | 3.20 or newer |
| GPU | Vulkan (Linux) or Metal (macOS) |
| System packages | **GLFW**, plus `wayland-devel` on Linux (or X11 headers with `PROJV_USE_X11=ON`) |
| Everything else | Submodules, built for you: bgfx/bx/bimg (+ shaderc), glm, spdlog, nlohmann/json, EnTT, Jolt, doctest |

```bash
sudo dnf install gcc-c++ cmake git glfw-devel wayland-devel     # Fedora
```

| Platform | Status |
|---|---|
| Linux | Primary. Wayland by default; `-DPROJV_USE_X11=ON` for X11. Vulkan backend. |
| macOS | Works, Metal backend, less exercised. |
| Windows | **Does not run.** `src/graphics/render_instance.cpp` never selects a renderer backend for it, so `bgfx::init` gets an unset type. |

## First build

```bash
git clone https://github.com/ajsuper/ProjectV.git
cd ProjectV
git submodule update --init --recursive

cmake --preset dev
cmake --build --preset dev
```

## Presets

| Preset | Build dir | For |
|---|---|---|
| `dev` | `build/` | Day to day. Submodule deps, `RelWithDebInfo`, examples and tests on. |
| `release` | `build-release/` | Optimised, optional log categories compiled out (`PROJV_LOG_MINIMAL=ON`), Jolt asserts off. |
| `vcpkg` | `build-vcpkg/` | Every dependency through `find_package` from a vcpkg toolchain. Needs `VCPKG_ROOT`. |

```bash
cmake --preset release && cmake --build --preset release
```

## Targets

| Target | What |
|---|---|
| `projectV` | The library. One target, `ProjectV::projectV` for consumers. |
| `hello_voxel`, `entities`, `sandbox`, ... | The [examples](../../examples/README.md). Target names are not directory names: `30-renderers` builds `renderer_gallery`. |
| `projv_unit_tests` | The unit tests. See [Testing](../guides/testing.md). |
| `docs`, `docs-serve` | This documentation as a web site. See [Writing docs](../contributing/writing-docs.md). |

## Options

| Option | Default | Effect |
|---|---|---|
| `PROJV_BUILD_EXAMPLES` | `OFF` (`ON` in `dev`) | Build the examples |
| `PROJV_BUILD_TESTS` | `OFF` (`ON` in `dev`) | Build `tests/unit/`; run with `ctest --preset dev` |
| `PROJV_BUILD_MANUAL_TESTS` | `OFF` | Build the windowed harnesses in `tests/manual/` |
| `PROJV_PHYSICS_ASSERTS` | `ON` (`OFF` in `release`) | Jolt's own assertions |
| `PROJV_INSTALL` | `ON` | Install and export rules |
| `PROJV_USE_X11` | `OFF` | X11 instead of Wayland for GLFW |
| `PROJV_LOG_MINIMAL` | `OFF` | Compile out every log category except `WARN` and `ERROR` |
| `PROJV_LOG_TRACE` `PERF` `EDIT` `RENDER` `INFO` `WARN` `ERROR` | `ON` | One switch per [log category](../guides/logging.md) |

**The options are `PUBLIC` on the target**, so consumers inherit them. The log macros compile call
sites out, so a consumer built with a different set behaves differently, not just logs differently.

## Running what you built

Examples land in `build/examples/<name>/` with their assets staged beside them. **Run each from its
own directory**: `resources.json` names shader paths relative to the working directory.

To be independent of the working directory, build asset paths from
`projv::core::executableDirectory()` ([`core/paths.h`](../../include/core/paths.h)):

```cpp
const auto assets = projv::core::executableDirectory() / "assets";
scene = projv::utils::loadComposeFromDisk((assets / "scenes/Castle").string());
```

## Shaders

`projv_compile_shaders()` ([`cmake/ProjectVShaders.cmake`](../../cmake/ProjectVShaders.cmake)) runs
bgfx's `shaderc` over `vs_*.sc` and `*.frag`, writing each `.bin` beside its source, with
`pjv_utils_DDA.sc` and bgfx's headers on the include path.

```cmake
projv_compile_shaders(TARGET my_app SHADER_DIRS myRenderer/shaders)
```

By hand:

```bash
build/bin/shaderc -f shader.frag -o shader.bin --type f \
    --platform linux --profile spirv -i external/bgfx/src -i include
```

## Using ProjectV from your own project

```bash
cmake --install build --prefix /path/to/prefix
```

```cmake
find_package(ProjectV CONFIG REQUIRED)
add_executable(my_game src/main.cpp)
target_link_libraries(my_game PRIVATE ProjectV::projectV)
projv_compile_shaders(TARGET my_game SHADER_DIRS myRenderer/shaders)
```

- Headers install under `<prefix>/include/projv/`; includes stay `"core/application.h"`.
- **A submodule build installs the vendored dependencies too.** Give it a prefix of its own, not one
  that already has bgfx or glm.
- A `vcpkg` build installs only ProjectV.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `bgfx was not found and external/bgfx.cmake is missing` | Submodules not checked out: `git submodule update --init --recursive`. |
| Configure skips `20-mesh-voxelizer` or `60-scene-editor` | They need their own submodules (Assimp; ImGui and Lua). Same command. |
| CMake error naming `external/bimg/3rdparty` or `l-smash` | `bgfx.cmake` has drifted from the `bgfx`/`bx`/`bimg` pins. The four move together: `git submodule update --init --recursive`. |
| An example renders nothing | Check the `Loaded N chunk(s)` line. Some bundled scenes are `.data` version 1, which the loader rejects. See [known latent issues](../plans/known-latent-issues.md). |
| Shaders don't rebuild after an edit | `projv_compile_shaders` tracks the sources and shared `.sc` files in the same directory only. Delete the `.bin` files to force it. |

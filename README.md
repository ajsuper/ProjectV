<p align="center">
  <img src="docs/images/Path_Trace_Renders/San_Miguel_Hallway.png" width="48%" />
  <img src="docs/images/Path_Trace_Renders/Amazon-Lumberyard-Sunset.png" width="48%" />
  <img src="docs/images/Path_Trace_Renders/San_Miguel_Hallway_Sun.png" width="88%" />
</p>

# ATTENTION!
ProjectV is under very early development, data structures and functionalities will change frequently so don't currently invest too much time into generating data or programs unless you plan on sticking with a version.

# ProjectV

'ProjectV' is a game engine that simplifies the process of making voxel games and allows for complex and effecient rendering.

# Contents

- [Goals of ProjectV](#Goals-of-ProjectV)
- [Installation](#installation)
- [Uninstallation](#uninstallation)
    - [Entire library](#entire-library)
    - [Only build files](#only-build-files)
- [Dependencies](#dependencies)
    - [Install Dependencies](#install-dependencies)
- [Modules](#modules)
    - [Utils](#utils)
    - [Core](#core)
    - [Graphics](#graphics)
- [Usage](#usage)
- [Contributing](#contributing)

### Goals of ProjectV

---

Voxels have unique properties that make them ideal for advanced rendering while providing a unique and charming aesthetic. This project aims to:

- Allow inexperienced programmers to experiment with rendering beautiful scenes without being reliant on a complex or high level game engine.
- Allow experienced programmers to leverage the provided functionalities to achieve performant and graphically impressive scenes. 
- Simplify the process of rendering 3D scenes.
- Create an open source voxel engine to allow for people to test their ideas.

### Installation

---

Linux and macOS. Windows is not currently supported: the renderer backend is never selected for
it (`src/graphics/render_instance.cpp`), so it does not run even though the dependencies are all
cross-platform. On Linux the default GLFW backend is Wayland; configure with `-DPROJV_USE_X11=ON`
for X11.

Fedora:
```bash
sudo dnf install gcc-c++ cmake git glfw-devel wayland-devel

git clone https://github.com/ajsuper/ProjectV.git
cd ProjectV
git submodule update --init --recursive

cmake --preset dev
cmake --build --preset dev
```

The first build compiles bgfx, bx, bimg and shaderc from source, so it takes a while. Subsequent
builds do not.

Examples land in `build/examples/<name>/` with their renderer folders and scenes staged beside the
binary. Run them from there — the engine resolves relative paths against the working directory:

```bash
cd build/examples/scene_previewer && ./scene_previewer scenes/StonehillCastle
```

#### Build options

| Option | Default | Effect |
|---|---|---|
| `PROJV_BUILD_EXAMPLES` | `OFF` (`ON` in the `dev` preset) | Build the bundled examples |
| `PROJV_USE_X11` | `OFF` | X11 instead of Wayland for GLFW on Linux |
| `PROJV_LOG_MINIMAL` | `OFF` | Compile out every log category except WARN and ERROR |
| `PROJV_LOG_TRACE` / `PERF` / `EDIT` / `RENDER` / `INFO` / `WARN` / `ERROR` | `ON` | Individual log categories |
| `PROJV_INSTALL` | `ON` | Generate install/export rules (see below) |

Presets: `dev` (submodule dependencies, examples on), `release` (optimised, minimal logging),
`vcpkg` (dependencies via `find_package`).

**On installing.** `cmake --install build --prefix <dir>` produces a config package a consumer
finds with `find_package(ProjectV CONFIG REQUIRED)`. A submodule build installs the vendored
dependencies alongside ProjectV so the prefix is self-contained — give it a prefix of its own
rather than one that already carries bgfx or glm from elsewhere. A `vcpkg` build resolves every
dependency through `find_package` and installs only ProjectV.

### Uninstallation

---

#### Entire library (Completely deletes the library):

> **Warning** rm -r can be dangerous when used incorrectly. Ensure it is used on the desired file path.
```bash
rm -r /path/to/ProjectV
```

#### Only build files (Keeps the source code and file structure):

```bash
cd /path/to/projectV/build
make clean
```

### Dependencies

---

Vendored as submodules and built automatically — nothing to install:

- [bgfx / bx / bimg](https://github.com/bkaradzic/bgfx) — GPU abstraction (Vulkan on Linux, Metal on macOS), built through [bgfx.cmake](https://github.com/bkaradzic/bgfx.cmake), which also builds the `shaderc` used to compile shaders
- [glm](https://github.com/g-truc/glm) — maths
- [spdlog](https://github.com/gabime/spdlog) — logging
- [nlohmann/json](https://github.com/nlohmann/json) — renderer and scene descriptions

From the system:

- A C++17 compiler, CMake ≥ 3.20, git
- **GLFW** — the one hard system dependency (`glfw-devel` on Fedora), plus `wayland-devel` on Linux
- Up-to-date graphics drivers with a Vulkan (Linux) or Metal (macOS) capable GPU

Two examples carry submodules of their own and are skipped, with a message, when they are not
checked out: MeshVoxelizer needs Assimp, and SceneEditor needs ImGui and Lua. `git submodule
update --init --recursive` gets everything.

### Modules

ProjectV is organised into four groups of headers under `include/`, all built into a single
library. There is one target to link, `ProjectV::projectV` — the per-module archives and their
hand-ordered link lists are gone.

| Group | Header example | What it provides |
|---|---|---|
| **Core** | `#include "core/ecs.h"` | `projv::Application`, the system stages and the game loop, maths, logging, `executableDirectory()`. See [core.md](/include/core/core.md) |
| **Utils** | `#include "utils/voxel_math.h"` | Voxel data: scene composition and I/O, materials, editing, picking, animation, scene queries. See [utils.md](/include/utils/utils.md) |
| **Graphics** | `#include "graphics/render_instance.h"` | Window and bgfx setup, renderer specifications, GPU upload, the render loop. See [graphics.md](/include/graphics/graphics.md) |
| **Data structures** | `#include "data_structures/scene.h"` | The plain types the three above operate on — `Scene`, `Chunk`, `GPUData`, `RendererSpecification` |

Everything lives under the `projv` namespace, with `projv::core`, `projv::utils` and
`projv::graphics` for the three functional groups.

### Usage

With ProjectV installed, a consumer needs no include or library paths of its own:

```cmake
find_package(ProjectV CONFIG REQUIRED)

add_executable(my_game src/main.cpp)
target_link_libraries(my_game PRIVATE ProjectV::projectV)

# Compiles this renderer's .sc/.frag shaders with bgfx's shaderc, against the engine's own
# shader library so pjv_utils_DDA.sc resolves.
projv_compile_shaders(TARGET my_game SHADER_DIRS myRenderer/shaders)
```

A minimal application registers its stages and runs the loop:

```cpp
#include "core/ecs.h"
#include "graphics/render_instance.h"

void startup(projv::Application& app) {
    auto& renderInstance =
        projv::core::createGlobalResource<projv::graphics::RenderInstance>(app.world);
    renderInstance.initialize(1920, 1080, "My Game");
    // ... load a scene, build a renderer, upload it to the GPU
}

void update(projv::Application& app) {
    auto& renderInstance =
        projv::core::getGlobalResource<projv::graphics::RenderInstance>(app.world);
    // The engine records the window-manager close request; acting on it is yours to decide.
    if (renderInstance.shouldClose) app.closeAppFlag = true;
}

int main() {
    projv::Application app = projv::core::createApp();
    projv::core::assignSystemStage(app, projv::SystemStage::Startup, startup);
    projv::core::assignSystemStage(app, projv::SystemStage::Update,  update);
    projv::core::assignSystemStage(app, projv::SystemStage::Render,  render);
    projv::core::runApplication(app);
}
```

Assets are ordinary paths. The engine's load functions open what they are given, resolving a
relative path against the working directory, so an application whose assets sit beside its binary
composes them from `projv::core::executableDirectory()`:

```cpp
const auto assets = projv::core::executableDirectory() / "assets";
scene = projv::utils::loadComposeFromDisk((assets / "scenes/Castle").string());
```

For worked examples, see [the examples](/docs/examples).

### Contributing

This is my first time open sourcing any project, and my first project on this scale. If there is anything that I should've done differently, or if something could be greatly improved, please feel free to change it or let me know! ProjectV is open source under the [MIT License](/LICENSE.md) and is welcoming as many contributors as possible! All contributions are welcome, whether it's features additions, optimizations, or just spelling corrections. If you choose to contribute, please visit the [MIT License](/LICENSE.md) and make sure you read the [CODE_OF_CONDUCT.md](/CODE_OF_CONDUCT.md). Also be sure to visit [CONTRIBUTING.md](/CONTRIBUTING.md) to get instructions on how to contribute! Thank you!!

#### Citations

Contributors must cite sources that require it in our [SOURCES.md](/SOURCES.md). You are not required to cite ProjectV when using it; however, it is highly recommended as the credit is appreciated and it helps people find out about the engine!

Thank you for using ProjectV!!

# Windows

Status: **in progress**, 2026-10-26. W0 done; W1 running (first CI build pending).

Goal: ProjectV builds, passes its tests and draws on Windows 10/11, with a CI check that keeps it
that way. Nobody develops on Windows, so **GitHub's `windows-latest` runner is the Windows machine**
for everything that needs no GPU, and one Windows machine with a real GPU, used rarely, is for the
rest.

## Where it started

`docs/getting-started/build.md` said Windows "does not run". True, and for one reason at the surface:
`RenderInstance::initialize` never gave bgfx a native window or a backend. Underneath, nothing had been
compiled with MSVC, so the rest was unknown. What was already there: ENet linked `ws2_32`/`winmm`,
`projv_compile_shaders` knew a `windows` platform, `core/paths.cpp` had a `_WIN32` branch, MSVC flags
(`/W4`, `/fp:precise`) were set, `GLFW_EXPOSE_NATIVE_WIN32` was in the header.

## Decisions

| | Decision | Why |
|---|---|---|
| D1 | **MSVC** (`cl`) is the target compiler; code is made portable rather than the compiler made to pass for GCC | It is what a Windows developer has; clang-cl would hide the GCC-isms this fixes |
| D2 | **Vulkan on Windows**, the same SPIR-V shaders as Linux | One shader backend. A Direct3D path would be a second HLSL translation of every shader, with its own bugs |
| D3 | CI first builds **the library and the unit tests**; examples follow once those are green | Everything else depends on them, and a runner has no GPU to draw with |
| D4 | The golden physics recording must replay bit for bit on MSVC, as on GCC and Clang | That is the claim "every build replays it". If it fails the finding is a decision for the maintainer, not something to work around |
| D5 | Dependencies: the submodules, except GLFW, which `vcpkg` provides on the runner | Linux takes GLFW from the system; there is no GLFW submodule |
| D6 | The example self-tests that are bash scripts (`selftest.sh`, `scale.sh`) are not run on Windows | They need a POSIX shell. Their C++ halves (`net_sockets`, the replication tests) do run |

## Milestones

| | What | Done when |
|---|---|---|
| **W0** | Portable code: `std::popcount` / `std::countl_zero` for the GCC bit intrinsics (about 25 sites), `SetThreadDescription` for the thread names, the Win32 window handle and Vulkan in `render_instance.cpp`, bash tests skipped on Windows, a `windows` configure preset, `.github/workflows/windows.yml` (not yet a gate) | Linux still builds and passes `ctest --preset dev`; the workflow exists |
| **W1** | The library and `projv_unit_tests` compile with MSVC | The workflow's Build step is green |
| **W2** | The unit tests pass on Windows, **including the golden recordings** (D4) | The workflow's Test step is green; `continue-on-error` is removed and it gates pull requests |
| **W3** | The examples build on Windows: their submodules in CI, shaders compiled by `shaderc` for the `windows` platform, scripts and paths | The workflow builds `--preset windows` with `PROJV_BUILD_EXAMPLES=ON` |
| **W4** | It draws: on the Windows machine with a GPU, `hello_voxel`, the renderer gallery, Voxel Mayhem and the terrain generator | A checklist below, run by hand and recorded here |
| **W5** | `build.md` and `README` say Windows works, and how to build it; release builds, if wanted | Docs match what was tried |

## The hand check (W4)

For when the Windows machine is available. Build, then run each from its own directory (as on Linux):

1. `cmake --preset windows -DPROJV_BUILD_EXAMPLES=ON` then `cmake --build --preset windows`, from a
   Developer Command Prompt for VS, with `-DCMAKE_PREFIX_PATH` at vcpkg's `installed/x64-windows`.
2. `hello_voxel`: a window, a shape, mouse look. Resize it. Close it with the button and with Esc.
3. `renderer_gallery --renderer restir-pt` and `--renderer fast`: the images match Linux's by eye.
4. `voxel_mayhem_demo` with `MAYHEM_SELFTEST=1`, then play it a minute (fire, break, walk).
5. `terrain_generator`: terrain streams in, the sun keys work.
6. Record the GPU, driver, and anything that differs from Linux below.

## Log

- **2026-10-26 (W0):** the bit intrinsics, thread name, window handle, test list and workflow.
  Linux unchanged: 3 of 3 ctest, the build clean. Not yet seen on a Windows compiler.

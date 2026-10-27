# Windows

Status: **in progress**, 2026-10-26. **W0-W2 done**: the library and all 380 unit tests build and pass on MSVC (CI), the golden recordings included. W2's last step, making the Windows job a gate, waits on repeat runs (below). W3 (examples) next.

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
| **W1** | The library and `projv_unit_tests` compile with MSVC | The workflow's Build step is green. **Done** |
| **W2** | The unit tests pass on Windows, **including the golden recordings** (D4) | The workflow's Test step is green. **Done**; `continue-on-error` is removed (so it gates pull requests) once repeat runs show no flake |
| **W3** | The examples build on Windows: their submodules in CI, shaders compiled by `shaderc` for the `windows` platform, scripts and paths | The workflow builds `--preset windows` with `PROJV_BUILD_EXAMPLES=ON` |
| **W4** | It draws: on the Windows machine with a GPU, `hello_voxel`, the renderer gallery, Voxel Mayhem and the terrain generator | A checklist below, run by hand and recorded here |
| **W5** | `build.md` and `README` say Windows works, and how to build it; release builds, if wanted | Docs match what was tried |

## The hand check (W4)

For when the Windows machine is available. Build, then run each from its own directory (as on Linux):

1. `cmake --preset windows -DPROJV_BUILD_EXAMPLES=ON` then `cmake --build --preset windows`, from a
   Developer Command Prompt for VS, with `-DCMAKE_PREFIX_PATH` at vcpkg's `installed/x64-windows`.
2. `hello_voxel`: a window, a shape, mouse look (Esc frees the cursor; it does not quit). Resize it. Close it with the window button.
3. `renderer_gallery --renderer restir-pt` and `--renderer fast`: the images match Linux's by eye.
4. `voxel_mayhem_demo` with `MAYHEM_SELFTEST=1`, then play it a minute (fire, break, walk).
5. `terrain_generator`: terrain streams in, the sun keys work.
6. Record the GPU, driver, and anything that differs from Linux below.

## Log

- **2026-10-26 (W0):** the bit intrinsics, thread name, window handle, test list and workflow.
  Linux unchanged: 3 of 3 ctest, the build clean. Not yet seen on a Windows compiler.
- **2026-10-26 (W1, W2):** four rounds of CI fixed what MSVC found. Everything is in the commit log; the
  findings worth keeping:
  - *windows.h defines `near` and `far` as macros*: every variable with either name is renamed
    (`nearby`, `farther`, ...), and the library defines `NOMINMAX` and `WIN32_LEAN_AND_MEAN` on Windows.
  - *Jolt links the static C runtime by default*: `USE_STATIC_MSVC_RUNTIME_LIBRARY` is OFF so it matches the rest.
  - *Windows will not delete an open file*: two tests removed a folder with an `ifstream` still open.
  - **D4, the golden recordings: the engine is deterministic across Linux/GCC and Windows/MSVC.** The Linux
    recording replays bit for bit on MSVC, Jolt and the replay included. What differed was a *fresh* recording
    of the session on MSVC: a strike's reach used `std::cbrt`, whose last bit differs between glibc and the
    Windows C runtime, and the reach sets the push falloff, so each shard's starting velocity. `cubeRoot` in
    `physics.cpp` is now `+ - * /` and `frexp`/`ldexp` only, both recordings were regenerated, and MSVC then
    matched the Linux recording byte for byte. **The rule this adds to the physics notes: nothing that feeds a
    step calls libm** (`sin`, `cos`, `pow`, `exp`, `cbrt`, ...). Known left: `glm::angleAxis` in the
    `ConstantVelocity` spin, which is libm and feeds a kinematic pose; nothing tests it across platforms yet.
  - `test_physics_golden.cpp` now says where a recording first differs and, with `PROJV_GOLDEN_OUT`, saves it;
    the Windows workflow uploads it. That is how the cause was found without a Windows machine.
  - One run crashed inside `test_physics_loading` ("the floor streams under a walking character", an unhandled
    SEH exception, no stack) and did not repeat on a re-run of the same commit or in repeated Linux runs. Unexplained;
    if it returns, an AddressSanitizer build on Windows is the next step.

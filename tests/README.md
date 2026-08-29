# Tests

There is no automated test suite yet. What exists is `manual/` — harnesses that need a display and
a GPU, run by hand.

[`docs/plans/test-suite-notes.md`](../docs/plans/test-suite-notes.md) is the design input for a
real suite: what should be covered, organised by subsystem, with the confirmed failure mode for
each entry and notes on the mistakes made while writing the harnesses that do exist.

## Running them

```bash
cmake --preset dev -DPROJV_BUILD_MANUAL_TESTS=ON
cmake --build --preset dev
```

| Harness | Built by | What it checks |
|---|---|---|
| [`manual/exit_path`](manual/) | the `Makefile` beside it | The application lifecycle: a clean exit with and without a registered Shutdown stage, and that `RenderInstance::shouldClose` tracks the window manager. |
| [`manual/paths_probe`](manual/) | the `Makefile` beside it | `executableDirectory()` returns the same real directory however the binary was invoked. |
| [`manual/editing_p1`](manual/editing_p1/) | CMake | 86 self-checks over the editing API. CPU only, no window. |
| [`manual/edit_demo`](manual/edit_demo/) | CMake | Interactive voxel editing on the GPU. |

`exit_path` and `paths_probe` are built by a hand-written Makefile rather than by CMake on purpose:
`exit_path` exists partly to check the engine behaves for a consumer that is *not* using ProjectV's
own CMake helpers.

```bash
cd tests/manual && make
./exit_path a && ./exit_path b
./paths_probe
```

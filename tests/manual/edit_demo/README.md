# edit_demo

Interactive voxel editing on the GPU: fly around a loaded scene and add or remove voxels under the
cursor. It exercises the edit → `updateScene` → incremental GPU upload path with a human driving
it, which is the part `editing_p1` cannot cover.

**This is a test harness, not an example.** It used to live under the examples. For editing shown
as part of a real application, see [60-scene-editor](../../../examples/60-scene-editor/); for
editing applied to generated terrain, see
[50-terrain-generator](../../../examples/50-terrain-generator/).

## Running

```bash
cmake --preset dev -DPROJV_BUILD_MANUAL_TESTS=ON
cmake --build --preset dev --target edit_demo

cd build/examples/edit_demo
./edit_demo
```

| Input | Action |
|-------|--------|
| `W` `A` `S` `D` / `R` `F` | Fly |
| Mouse | Look around |
| `E` | Add a voxel (cycles colours) |
| `Q` | Remove a voxel |
| `1`–`4` | Edit distance |

## Current state

Its bundled `SponzaScene/` is a `.data` version 1 container, which the current loader rejects — so
the scene loads as **zero chunks** and there is nothing to edit. It runs, but it cannot do its job
until that asset is re-voxelized. See
[`docs/plans/known-latent-issues.md`](../../../docs/plans/known-latent-issues.md).

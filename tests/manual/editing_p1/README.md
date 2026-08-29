# editing_p1

A CPU-only driver for the editing API. No window, no shaders, no GPU — it loads a scene, runs 86
self-checks over `queueVoxelAdd` / `queueVoxelRemove` / `updateScene` and the grid machinery
underneath them, and prints `[OK]` or `CHECK FAILED` for each.

**This is a test harness, not an example.** It used to live under the examples, which is why it
reads like one in places. Nothing here is meant to demonstrate how to write an application.

## Running

```bash
cmake --preset dev -DPROJV_BUILD_MANUAL_TESTS=ON
cmake --build --preset dev --target editing_p1

cd build/examples/editing_p1
./editing_p1 [scene-directory]
```

It needs a scene with actual geometry. Its default —
`../renderer_gallery/SponzaScene/` — is currently a `.data` version the loader rejects, so it
exits with a message rather than a segfault. Point it somewhere that loads:

```bash
./editing_p1 ../scene_previewer/scenes/SmallVox
```

## Current state

**80 of 86 checks pass.** Six fail:

```
Sponza grid dims unchanged after in-bounds edit
programmatic: seed has 5 voxels
programmatic: voxel count grew by 3
p3-convert: cell 0 has 6 voxels (5 seed + 1 add)
grid-test: chunk 0 has 5 voxels (3 seed + 2 adds)
grid-test: chunk 1 has 3 voxels (2 seed + 1 add)
```

The first is expected: it names Sponza, and Sponza cannot currently be loaded, so a substitute
scene has different dimensions. The other five build **synthetic** scenes inside the driver and are
independent of which scene was loaded — they are genuine voxel-count failures in the editing
subsystem.

They are not new. Verified by rebuilding against the engine with every recent change stashed:
identical results. They were most likely unnoticed because the driver's default scene stopped
loading, so it could not be run at all without being handed a different scene by hand.

See [`docs/plans/known-latent-issues.md`](../../../docs/plans/known-latent-issues.md).

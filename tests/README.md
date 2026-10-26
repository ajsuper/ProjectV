# Tests

Two kinds:

- **`unit/`**: CPU only, [doctest](https://github.com/doctest/doctest), built in the `dev` preset (`PROJV_BUILD_TESTS`).
- **`manual/`**: need a display and a GPU.

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

| Suite | What it checks |
|---|---|
| [`unit/test_application.cpp`](unit/test_application.cpp) | `Application`: stage order, system order, fixed steps before Update, pausing, Shutdown once. |
| [`unit/test_time.cpp`](unit/test_time.cpp) | `Time`: the first advance, the fixed-step accumulator, `maxDelta` clamping, dropped steps. |
| [`unit/test_events.cpp`](unit/test_events.cpp) | `Events`: queued delivery at the pump, handler order, sends during a pump wait a frame, `trigger`. |
| [`unit/test_input.cpp`](unit/test_input.cpp) | `Input`: pressed/released edges last exactly one frame; keys and buttons independent. |
| [`unit/test_attachments.cpp`](unit/test_attachments.cpp) | Component attachments: typed and raw access, decode on first use, versions, unknown keys surviving save, duplicate/clear policies, Scene copy/move, grafting, the legacy `op` migration. |
| [`unit/test_local_ids.cpp`](unit/test_local_ids.cpp) | `localId`: sibling numbering, saved and reloaded, missing and duplicate ids renumbered. |
| [`unit/test_scene_delete.cpp`](unit/test_scene_delete.cpp) | `deleteComponent`: chunks, grids and asset subtrees removed, geometry released, records tombstoned. |
| [`unit/test_slot_reuse.cpp`](unit/test_slot_reuse.cpp) | Slot reuse: freed rows reused, `ComponentRef` noticing the new occupant, recycling off, tables bounded over long spawn/destroy runs, the root index. |
| [`unit/test_voxel_edits.cpp`](unit/test_voxel_edits.cpp) | Edits that remove nothing change nothing; a chunk carved to nothing uploads as empty. |
| [`unit/test_loose_bvh.cpp`](unit/test_loose_bvh.cpp) | The loose-chunk BVH against brute force: content bounds, world bounds, ray traversal, overlaps. |
| [`unit/test_collision_geometry.cpp`](unit/test_collision_geometry.cpp) | Collision pieces cover the voxels exactly, deterministically, with the budget fallback and content stamps. |
| [`unit/test_scene_bridge.cpp`](unit/test_scene_bridge.cpp) | The Scene bridge: links seed and sync Transforms, Root/Part modes, refused links, deleted components. |
| [`unit/test_entities.cpp`](unit/test_entities.cpp) | `entities.json`: links by id, standalone entities, nested-folder overrides, unknown components kept, save round trips. |
| [`unit/test_physics_world.cpp`](unit/test_physics_world.cpp) | `PhysicsWorld`: settling, bit-for-bit determinism, snapshot restore, refusing bad input. |
| [`unit/test_physics_runtime.cpp`](unit/test_physics_runtime.cpp) | Physics on entities: crates resting on floors, nothing solid unasked, interpolated presentation, command order. |
| [`unit/test_physics_edits.cpp`](unit/test_physics_edits.cpp) | Voxel islands, bodies split by edits, shapes rebuilt, floors carved or deleted under sleeping bodies. |
| [`unit/test_physics_fracture.cpp`](unit/test_physics_fracture.cpp) | Fracture: no voxel lost or made, palette entries kept, reach follows energy, patterns, repeatability. |
| [`unit/test_physics_materials.cpp`](unit/test_physics_materials.cpp) | Physics materials against closed forms: slide distance, bounce height, mixed-body mass and centre of mass. |
| [`unit/test_physics_joints.cpp`](unit/test_physics_joints.cpp) | Joints: each type against its physics, limits, motors, breaking, determinism and rewind. |
| [`unit/test_physics_character.cpp`](unit/test_physics_character.cpp) | Characters: standing, walking, steps, walls, slopes, jumping, pushing, moving ground, replay. |
| [`unit/test_physics_queries.cpp`](unit/test_physics_queries.cpp) | Rays, sphere casts and overlaps, with hits traced to the same voxel `pickVoxel` finds. |
| [`unit/test_physics_net.cpp`](unit/test_physics_net.cpp) | Networking readiness: exact replays, `NetId` ordering, stamped commands, runtime rewind. |
| [`unit/test_physics_reliability.cpp`](unit/test_physics_reliability.cpp) | Hard cases: thread-count independence, tunnelling, tall stacks, big piles, blow-ups, leaving the world; the soak (`ctest --preset soak`). |

Two ctests are not compiled:

| Test | What it checks |
|---|---|
| `layering` ([`unit/check_layering.cmake`](unit/check_layering.cmake)) | `utils/` and `data_structures/` never include EnTT or the runtime; only `src/runtime/physics/` includes Jolt. |
| `docs` ([`tools/docs/check_links.py`](../tools/docs/check_links.py)) | Every docs link and anchor resolves, every page is reachable from `docs/README.md`, every header is named in its module page. |

What the suite should cover, by subsystem and with confirmed failure modes:
[`docs/plans/test-suite-notes.md`](../docs/plans/test-suite-notes.md).

## Running them

```bash
cmake --preset dev -DPROJV_BUILD_MANUAL_TESTS=ON
cmake --build --preset dev
```

| Harness | Built by | What it checks |
|---|---|---|
| [`manual/exit_path`](manual/) | the `Makefile` beside it | The application lifecycle: a clean exit with and without a registered Shutdown stage, and that `RenderInstance::shouldClose` tracks the window manager. |
| [`manual/paths_probe`](manual/) | the `Makefile` beside it | `executableDirectory()` returns the same real directory however the binary was invoked. |
| [`manual/editing_p1`](manual/editing_p1/) | CMake | 86 self-checks over the editing API (CPU only). |
| [`manual/edit_demo`](manual/edit_demo/) | CMake | Interactive voxel editing on the GPU. |

`exit_path` and `paths_probe` use a hand-written Makefile on purpose: they check the engine works
for a consumer not using ProjectV's CMake helpers.

```bash
cd tests/manual && make
./exit_path a && ./exit_path b
./paths_probe
```

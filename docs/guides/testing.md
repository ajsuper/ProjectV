# Testing

Folders: [tests/README.md](../../tests/README.md). Coverage wishlist:
[plans/test-suite-notes.md](../plans/test-suite-notes.md).

| What | Command | Needs |
|---|---|---|
| Unit tests + layering + docs links | `ctest --preset dev` | CPU only |
| Physics soak (2000 bodies, ten simulated minutes, ~30 s) | `ctest --preset soak` | After changing physics |
| One test case, verbose | `build/bin/projv_unit_tests -tc="*fixed step*" -s` | |
| Sandbox self-test | `cd build/examples/sandbox && SANDBOX_SELFTEST=600 ./sandbox` | A display |
| Physics demos self-test | `cd build/examples/physics_demos && DEMOS_SELFTEST=1 ./physics_demos` | A display |
| Manual harnesses | `cmake --preset dev -DPROJV_BUILD_MANUAL_TESTS=ON` | A display |

## The ctest tests

| Test | What it is |
|---|---|
| `projv_unit_tests` | The doctest suite below |
| `physics_soak` | The same binary, `-tc=soak*` with `PROJV_SOAK=1`; label `soak`, excluded from `dev` |
| `layering` | A scan of includes: the voxel layer never includes EnTT or the runtime, only `src/runtime/physics/` includes Jolt ([`check_layering.cmake`](../../tests/unit/check_layering.cmake)) |
| `docs` | Every docs link resolves, every page is reachable, every header is in its module page ([`check_links.py`](../../tools/docs/check_links.py)) |

## The unit suite

[doctest](https://github.com/doctest/doctest), [`tests/unit/`](../../tests/unit/):

| File | Checks |
|---|---|
| `test_application.cpp` | Stages, system ordering, the frame loop, resources in `world.ctx()` |
| `test_time.cpp` | The frame clock and the fixed-step accumulator, exactly |
| `test_events.cpp` | Queued delivery at one point per frame, sends during a pump |
| `test_input.cpp` | Per-frame key and button edges |
| `test_attachments.cpp` | Attachments: typed and raw, versions, unknown keys surviving save, duplicate and graft |
| `test_local_ids.cpp` | `localId`: unique among siblings, saved as `"id"`, what entity files link by |
| `test_slot_reuse.cpp` | Freed rows reused, `ComponentRef` telling old from new, the root index |
| `test_scene_delete.cpp` | `deleteComponent` and `isComponentAlive` over subtrees |
| `test_voxel_edits.cpp` | Edits that remove nothing change nothing; a chunk carved empty is known empty |
| `test_loose_bvh.cpp` | The loose-chunk BVH against brute force on random scenes |
| `test_collision_geometry.cpp` | What voxels collide as, checked exactly against the voxels |
| `test_scene_bridge.cpp` | Entities linked to components through real frames |
| `test_entities.cpp` | `entities.json`: spawn, links by id, unknown components kept, save |
| `test_physics_world.cpp` | The physics core: settles, is deterministic, restores exactly, refuses bad input |
| `test_physics_runtime.cpp` | `RigidBody` / `StaticCollider` on linked components through real frames |
| `test_physics_edits.cpp` | Islands, splitting, shapes rebuilt on edit, floors carved under sleeping piles |
| `test_physics_fracture.cpp` | Strikes: no voxel lost or made, reach follows energy, patterns decide shards |
| `test_physics_materials.cpp` | The material library: looks and physics separate, contacts use the touched material |
| `test_physics_queries.cpp` | Rays, sphere casts, overlaps, traced to the voxel and checked against `pickVoxel` |
| `test_physics_joints.cpp` | Each joint type against closed forms, breaking, determinism |
| `test_physics_character.cpp` | Characters: stairs, slopes, walls, jumping, pushing, moving ground |
| `test_physics_net.cpp` | Recordings replay exactly, NetIds make peers agree, stamped commands, rewind |
| `test_physics_reliability.cpp` | Fast small things, tall stacks, big piles, blow-ups, things flung out; the soak |

## Adding a test

- Add `tests/unit/test_<subject>.cpp` to `add_executable(projv_unit_tests ...)` in
  [`tests/unit/CMakeLists.txt`](../../tests/unit/CMakeLists.txt) (no glob).
- `main.cpp` holds `DOCTEST_CONFIG_IMPLEMENT`; don't define it elsewhere.

```cpp
// Palettes: interning dedupes, and an entry's physics material keeps it apart.
#include "doctest/doctest.h"

#include "data_structures/scene.h"
#include "utils/material.h"
#include "utils/scene_query.h"

TEST_CASE("interning a colour twice gives one slot") {
    projv::Scene scene;
    auto h = projv::utils::addComponent(scene, projv::ComponentKind::Chunk, "c",
                                        projv::INVALID_COMPONENT_HANDLE, 16, 1.0f);
    auto& comp = scene.components[h];
    uint8_t a = projv::utils::internMaterial(scene, comp, "red", projv::packRGB10(1, 0, 0));
    uint8_t b = projv::utils::internMaterial(scene, comp, "red", projv::packRGB10(1, 0, 0));
    CHECK(a == b);
}
```

Patterns the suite uses:

- **Frames without a display**: replace `app.clock`, then `runStartup()`, `runFrame()`..., `runShutdown()`.
- **Scenes without files**: `addComponent` + `queueVoxelAdd` + `updateScene`.
- **Determinism**: compare `PhysicsWorld::stateHash()` (or `runtime::physicsStateHash(world)`) step
  by step, or set `PhysicsSettings::record` and require `PhysicsWorld::replay(...)` to report `matched`.
- **Exact where the answer is known** (voxel counts, slots, hashes); `doctest::Approx` against closed forms.

## Self-tests in the examples

Unattended runs that log `PASS` or `FAIL`.

| Example | Run | Checks |
|---|---|---|
| [`16-sandbox`](../../examples/16-sandbox/) | `SANDBOX_SELFTEST=600 ./sandbox` (3000 is the better physics check) | Every body accounted for, the walker never sinks, popped voxels gone, no dropped contacts, and the recording replays bit-identically. `SANDBOX_RECORD=<path>` writes the recording. |
| [`17-physics-demos`](../../examples/17-physics-demos/) | `DEMOS_SELFTEST=1 ./physics_demos` (`DEMOS_ONLY=9` or `2,11`: some) | Each demo does what it should; no dropped contacts, no non-finite bodies, nothing through the floor, the recording replays |

## Manual harnesses

In [`tests/manual/`](../../tests/manual/), built with `-DPROJV_BUILD_MANUAL_TESTS=ON` or the
`Makefile` there:

| Harness | Checks |
|---|---|
| `exit_path` | Clean exit with and without a Shutdown stage; `RenderInstance::shouldClose` tracks the window manager. Hand-written Makefile on purpose: a consumer not using ProjectV's CMake. |
| `paths_probe` | `executableDirectory()` agrees however the binary is invoked |
| `editing_p1` | 86 self-checks over the editing API (CPU only) |
| `edit_demo` | Interactive voxel editing on the GPU |

```bash
cd tests/manual && make && ./exit_path a && ./exit_path b && ./paths_probe
```

# Coding style

How the C++ in this repository is actually written. When this page and the code disagree, match the
surrounding code and fix this page.

## The language

- **C++20.** Designated initializers, `= default` comparisons, `if constexpr`, `std::optional` are
  all in use.
- **`-Wall -Wextra`** on the engine. Keep it warning-free.
- **One library**, `projectV`, plus the `projv_physics` object library (the only code that includes
  Jolt). See [Architecture](docs/concepts/architecture.md) for what each module may include; the
  `layering` ctest enforces it.

## Names

| Thing | Style | Example |
|---|---|---|
| Types, enums, enum values | PascalCase | `ComponentRecord`, `LinkMode::Root` |
| Functions, variables, members | camelCase, no prefixes | `queueVoxelAdd`, `geometryPoolIndex` |
| Constants | UPPER_SNAKE `constexpr` | `BRICK_SIZE`, `INVALID_COMPONENT_HANDLE` |
| Macros | `PROJV_` + UPPER_SNAKE | `PROJV_ENABLE_PERF`, `PROJV_MAX_PASS_SAMPLERS` |
| Files | snake_case | `scene_query.h`, `physics_world.cpp` |
| Namespaces | `projv`, `projv::core` / `utils` / `graphics` / `runtime` | `namespace projv::utils {` |
| Header guards | `PROJECTV_<NAME>_H` or `PROJV_<MODULE>_<NAME>_H` | `PROJECTV_SCENE_QUERY_H` |
| Saved keys (JSON) | namespaced, dotted | `physics.rigidbody`, `projv.editor.csg` |

Name things for what they are, in words: `materialPalette`, not `matPal`; `findComponentByLocalId`,
not `lookup`.

## Layout

```cpp
namespace projv::utils {
    // What the function does, what it refuses, and the rule a caller must keep.
    ComponentHandle addComponent(Scene& scene, ComponentKind kind, const std::string& name,
                                 ComponentHandle parent, uint32_t resolution, float voxelScale);
}
```

- **4 spaces**, no tabs. Namespace bodies are indented.
- **Braces on the same line** (K&R). A one-line `if (...) return false;` needs none.
- **About 100 columns.** Wrap comments there too.
- **Includes**: standard headers, then third-party, then the engine's own, each block separated.
  Engine headers by module path in quotes: `#include "utils/compose_io.h"`.
- [`.clang-format`](.clang-format) describes this layout, but isn't run over the tree. Don't
  reformat a file you aren't otherwise changing.

## Data and functions

- **Plain structs with default member initializers**, and free functions that take them:
  `utils::setComponentTransform(scene, handle, ...)`, not `scene.setTransform(...)`. Most of the
  engine is this shape: `Scene`, `Chunk`, `RigidBody`, `PhysicsMaterial`.
- **Classes where something owns resources or guards an invariant**: `Application`, `Events`,
  `PhysicsWorld`, `RenderInstance`. Delete the copies and moves that would be unsafe and say why.
- **Small inline helpers and static factories on a struct are fine** (`PhysicsShape::box`,
  `Input::down`, `ComponentRef::operator==`).
- **Traits for extension points**: a program teaches the engine its types by specialising
  `ComponentTraits<T>` or `AttachmentTraits<T>`, not by subclassing.
- **No globals.** State lives in a `Scene`, a `World`, or `world.ctx()`. The old
  `inline Camera cam` is the pattern not to repeat.

## Errors

The engine **refuses bad input, logs why, and carries on**. It doesn't assert and rarely throws.

| Situation | Do |
|---|---|
| Bad argument, dead handle, impossible request | Log (`core::warn` / `core::error`) and return `false`, `INVALID_*`, an empty result or `std::nullopt` |
| A result the caller must check | Say so in the comment (`**Check it.**`), or mark it `[[nodiscard]]` |
| Input that would reach Jolt | Refuse, clamp or log in `PhysicsWorld` first; never let Jolt assert on it |
| A file the program can't run without (a malformed `resources.json`) | Throw `std::runtime_error` with a message saying how to fix it |
| Repeated per-frame problems | Log once (`core_warn_every`, or a one-time flag), not every frame |

## Comments

The headers are the API reference: Doxygen reads them as they are
([`tools/docs/doxygen_filter.py`](tools/docs/doxygen_filter.py)), so there is no `///` markup to add.

```cpp
// Moves `child` from its current parent to `newParent`. Pass INVALID_COMPONENT_HANDLE to make it a
// root. Re-bakes the subtree's world transforms. `newParent` must not be a descendant of `child`
// (checked; returns false on a cycle).
bool setComponentParent(Scene& scene, ComponentHandle child, ComponentHandle newParent);
```

- **A `//` block directly above the declaration** documents it. Trailing `//` on a member
  documents that member. Older headers use `/** ... @param */` blocks; both are fine, don't convert.
- **Say what it does, what it refuses, and what the caller must keep true.** The rule a reader would
  otherwise learn by breaking it goes in **bold**.
- **Explain why when the obvious alternative is wrong**: "`glossiness` rather than roughness,
  because 0 has to mean fully rough". Not a history of how the code got here; that is the commit
  message's job.
- **Usage examples** are indented four spaces inside the comment.
- **Name the test or plan** that holds the reasoning when it is long (`see the physics plan, D4`).
- In `.cpp` files, comment the non-obvious step, not every line.

## Determinism

Anything that feeds a physics step, or is sent between machines:

- **No clocks, thread counts, `rand()` or unordered iteration.** Order by `NetId`, then by entity.
- **Never persist or send** an `Entity`, a handle, a `ComponentRef` or a body id. Files use
  `localId`; networks use `NetId`.
- Fracture patterns and anything else run inside a step take their randomness from what they are
  handed (`FractureRandom`).

## Logging

`core::trace` / `perf` / `edit` / `render` / `info` / `warn` / `error`, from
[`core/log.h`](include/core/log.h). Don't add the `[TAG]` yourself; throttle per-frame output with
the `core_<category>_every(N, ...)` / `_every_ms(ms, ...)` macros. Use these, not `std::cout`
(one stray remains in `voxel_management.cpp`). Details: [Logging](docs/guides/logging.md).

## Tests

A behaviour change comes with a doctest case in `tests/unit/`; a fix comes with the test that
would have caught it. See [tests/README.md](tests/README.md).

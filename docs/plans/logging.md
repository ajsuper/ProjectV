# Logging System — Compile-Time Category Toggles

## Goal

Replace the thin spdlog re-export in `include/core/log.h` with a category-based
logging system that:

- Is **header-only** — no `.cpp` file, no external state, no public frame counter.
- Has **compile-time category toggles** via CMake `option()` → preprocessor
  defines. When a category is disabled, the function body is empty → inlined →
  eliminated. Zero runtime overhead.
- Uses **short grep-able tags** (`[TRC]`, `[PRF]`, `[EDT]`, `[RND]`, `[INF]`,
  `[WRN]`, `[ERR]`) baked into the function, never added by the caller.
- **Does not manage frequency.** Every log function prints every time it is
  called. Throttling (every N frames, on-change only) is the caller's
  responsibility — the logging system does not know about frames.
- All categories **ON by default**. A `PROJV_LOG_MINIMAL` preset disables
  everything except WARN+ERROR for release builds.

## What exists (before this change)

- `include/core/log.h` — 15 lines. Re-exports spdlog free functions:
  ```cpp
  namespace projv::core {
      using spdlog::trace;
      using spdlog::debug;
      using spdlog::info;
      using spdlog::warn;
      using spdlog::error;
      using spdlog::critical;
  }
  ```
- spdlog is a git submodule at `external/spdlog`, added to the build via
  `add_subdirectory(external/spdlog)` in `CMakeLists.txt:18`.
- `link_common_includes()` in `CMakeLists.txt:61` adds `${SPDLOG_INCLUDE}` to
  every target, so all project code can `#include "spdlog/spdlog.h"`.
- Some source files already include `<fmt/format.h>` indirectly via spdlog.
- Manual tag conventions exist: `core::warn("[PERF] ...")` and
  `core::warn("[EDIT] ...")` are sprinkled through `src/utils/editing.cpp` and
  `src/graphics/gpu_interface.cpp`.
- A few files call `spdlog::` directly instead of going through `core::`:
  - `src/graphics/disk_io.cpp:149,167` — `spdlog::info(logFormat, ...)`
  - `docs/examples/edit_demo/main.cpp:340` — `spdlog::set_level(...)`
  - `docs/examples/PathTracer/main.cpp` — `spdlog::set_level(...)`
- CMakeLists.txt has **no project `option()` calls** and **no
  `add_compile_definitions`** — only external dependencies define options.

## Design Summary

Seven categories, each a variadic template function in `projv::core`:

| Category | Tag  | CMake Option         | Preprocessor Define  | Default | spdlog level |
|----------|------|----------------------|----------------------|---------|--------------|
| Trace    | `[TRC]` | `PROJV_LOG_TRACE`  | `PROJV_ENABLE_TRACE`  | ON      | `trace`      |
| Perf     | `[PRF]` | `PROJV_LOG_PERF`   | `PROJV_ENABLE_PERF`   | ON      | `info`       |
| Edit     | `[EDT]` | `PROJV_LOG_EDIT`   | `PROJV_ENABLE_EDIT`   | ON      | `info`       |
| Render   | `[RND]` | `PROJV_LOG_RENDER` | `PROJV_ENABLE_RENDER` | ON      | `info`       |
| Info     | `[INF]` | `PROJV_LOG_INFO`   | `PROJV_ENABLE_INFO`   | ON      | `info`       |
| Warn     | `[WRN]` | `PROJV_LOG_WARN`   | `PROJV_ENABLE_WARN`   | ON      | `warn`       |
| Error    | `[ERR]` | `PROJV_LOG_ERROR`  | `PROJV_ENABLE_ERROR`  | ON      | `error`      |

`critical` is dropped — there are zero call sites for it in the codebase, and
`error` covers the same ground. If it's ever needed, add it later following the
same pattern.

When `PROJV_ENABLE_<CAT>` is defined, the function formats the message with
fmt, prepends the tag, and forwards to spdlog. When not defined, the function
body is empty (`{}`), the compiler inlines it, and it's eliminated entirely —
no argument evaluation, no string formatting, no spdlog call.

The caller is responsible for any frequency throttling (every N frames, on
state change). The logging system has no knowledge of frames, counters, or
change-detection.

## Sub-steps

### Step 1 — Rewrite `include/core/log.h`

**File:** `include/core/log.h`

Replace the entire 15-line file with the new header. The full content:

```cpp
#ifndef PROJV_CORE_LOG_H
#define PROJV_CORE_LOG_H

#include "spdlog/spdlog.h"
#include <fmt/format.h>

namespace projv::core {

// ===== TRACE =====
#if defined(PROJV_ENABLE_TRACE)
template <typename... Args>
inline void trace(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::trace("[TRC] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#else
template <typename... Args>
inline void trace(fmt::format_string<Args...>, Args&&...) {}
#endif

// ===== PERF =====
#if defined(PROJV_ENABLE_PERF)
template <typename... Args>
inline void perf(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::info("[PRF] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#else
template <typename... Args>
inline void perf(fmt::format_string<Args...>, Args&&...) {}
#endif

// ===== EDIT =====
#if defined(PROJV_ENABLE_EDIT)
template <typename... Args>
inline void edit(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::info("[EDT] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#else
template <typename... Args>
inline void edit(fmt::format_string<Args...>, Args&&...) {}
#endif

// ===== RENDER =====
#if defined(PROJV_ENABLE_RENDER)
template <typename... Args>
inline void render(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::info("[RND] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#else
template <typename... Args>
inline void render(fmt::format_string<Args...>, Args&&...) {}
#endif

// ===== INFO =====
#if defined(PROJV_ENABLE_INFO)
template <typename... Args>
inline void info(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::info("[INF] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#else
template <typename... Args>
inline void info(fmt::format_string<Args...>, Args&&...) {}
#endif

// ===== WARN =====
#if defined(PROJV_ENABLE_WARN)
template <typename... Args>
inline void warn(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::warn("[WRN] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#else
template <typename... Args>
inline void warn(fmt::format_string<Args...>, Args&&...) {}
#endif

// ===== ERROR =====
#if defined(PROJV_ENABLE_ERROR)
template <typename... Args>
inline void error(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::error("[ERR] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#else
template <typename... Args>
inline void error(fmt::format_string<Args...>, Args&&...) {}
#endif

} // namespace projv::core
#endif // PROJV_CORE_LOG_H
```

**Why `fmt::format_string` instead of `const char*` or `spdlog::format_string`:**
spdlog bundles fmt and exposes `fmt::format_string` with compile-time format
string checking. Using it directly gives us type-safe `{}`-style formatting
matching the existing call-site style (`core::info("foo={}", x)`). The
`Args&&...` perfect-forwarding pack matches how spdlog itself declares these.

**Why `inline` on the templates:** These are defined in a header. Template
functions are implicitly inline, but marking them `inline` is harmless and
signals intent. More importantly, the empty-body `#else` branches **must** be
`inline` (or `static`) to avoid ODR violations across translation units. Since
they're templates, they're already implicitly inline — but keep the `inline`
keyword for clarity and consistency.

**Verify after writing:**
```bash
cmake -S . -B build && cmake --build build
```
This should fail to compile because existing callers use `core::warn("[EDIT] ...")`
which is still valid syntactically — but check for any breakage from the changed
signatures. If something breaks, fix it in Step 3 (migration).

### Step 2 — Add CMake options and defines

**File:** `CMakeLists.txt`

Insert the following block **after** line 20 (`add_subdirectory(external/glm)`)
and **before** line 22 (`set(SPDLOG_INCLUDE ...)`):

```cmake
# --- Logging categories (all ON by default) ---
option(PROJV_LOG_TRACE  "Enable trace logging"          ON)
option(PROJV_LOG_PERF   "Enable performance logging"     ON)
option(PROJV_LOG_EDIT   "Enable editing pathway logging" ON)
option(PROJV_LOG_RENDER "Enable rendering pathway logging" ON)
option(PROJV_LOG_INFO   "Enable info logging"            ON)
option(PROJV_LOG_WARN   "Enable warning logging"         ON)
option(PROJV_LOG_ERROR  "Enable error logging"           ON)

# Preset: disable everything except WARN+ERROR for release/production builds.
# Usage: cmake -DPROJV_LOG_MINIMAL=ON ..
option(PROJV_LOG_MINIMAL "Disable all optional logging (keep only WARN+ERROR)" OFF)
if(PROJV_LOG_MINIMAL)
    set(PROJV_LOG_TRACE  OFF CACHE BOOL "" FORCE)
    set(PROJV_LOG_PERF   OFF CACHE BOOL "" FORCE)
    set(PROJV_LOG_EDIT   OFF CACHE BOOL "" FORCE)
    set(PROJV_LOG_RENDER OFF CACHE BOOL "" FORCE)
    set(PROJV_LOG_INFO   OFF CACHE BOOL "" FORCE)
endif()

# Build the list of preprocessor defines for enabled categories
set(PROJV_LOG_DEFS "")
if(PROJV_LOG_TRACE)  list(APPEND PROJV_LOG_DEFS PROJV_ENABLE_TRACE)  endif()
if(PROJV_LOG_PERF)   list(APPEND PROJV_LOG_DEFS PROJV_ENABLE_PERF)   endif()
if(PROJV_LOG_EDIT)   list(APPEND PROJV_LOG_DEFS PROJV_ENABLE_EDIT)   endif()
if(PROJV_LOG_RENDER) list(APPEND PROJV_LOG_DEFS PROJV_ENABLE_RENDER) endif()
if(PROJV_LOG_INFO)   list(APPEND PROJV_LOG_DEFS PROJV_ENABLE_INFO)   endif()
if(PROJV_LOG_WARN)   list(APPEND PROJV_LOG_DEFS PROJV_ENABLE_WARN)   endif()
if(PROJV_LOG_ERROR)  list(APPEND PROJV_LOG_DEFS PROJV_ENABLE_ERROR)  endif()

# Apply to all targets in this directory and subdirectories
add_compile_definitions(${PROJV_LOG_DEFS})

message(STATUS "ProjectV logging categories: ${PROJV_LOG_DEFS}")
```

**Why `add_compile_definitions` instead of per-target `target_compile_definitions`:**
All project targets should see the same logging config. Since
`link_common_includes` is called per-target but doesn't set defines, using
directory-scoped `add_compile_definitions` is the simplest way to ensure
every `.cpp` file sees the defines. External submodules (spdlog, json, glm)
are added via `add_subdirectory` **before** this block, so they won't inherit
these defines (which is correct — they don't use `core::` logging).

**Why the `message(STATUS ...)`:** So the developer can see which categories are
active in the CMake configure output. Helps debugging.

**Verify after writing:**
```bash
rm -rf build && cmake -S . -B build 2>&1 | grep "ProjectV logging"
```
Expected output:
```
-- ProjectV logging categories: PROJV_ENABLE_TRACE;PROJV_ENABLE_PERF;PROJV_ENABLE_EDIT;PROJV_ENABLE_RENDER;PROJV_ENABLE_INFO;PROJV_ENABLE_WARN;PROJV_ENABLE_ERROR
```

Test the minimal preset:
```bash
rm -rf build && cmake -S . -B build -DPROJV_LOG_MINIMAL=ON 2>&1 | grep "ProjectV logging"
```
Expected output:
```
-- ProjectV logging categories: PROJV_ENABLE_WARN;PROJV_ENABLE_ERROR
```

### Step 3 — Migrate existing call sites

This is the largest step by volume but each change is mechanical. Work file by
file. The table below shows the transformation rules:

| Old pattern | New pattern | Notes |
|---|---|---|
| `core::info("...")` | `core::info("...")` | No change — just goes through new tagged template. |
| `core::warn("...")` | `core::warn("...")` | Same — but check for manual `[PERF]`/`[EDIT]` tags below. |
| `core::error("...")` | `core::error("...")` | No change. |
| `core::trace("...")` | `core::trace("...")` | No change. |
| `core::debug("...")` | `core::trace("...")` | `debug` no longer exists; map to `trace`. |
| `core::critical("...")` | `core::error("...")` | `critical` dropped; map to `error`. |
| `core::warn("[PERF] ...")` | `core::perf("...")` | Remove the manual `[PERF] ` prefix; use the `perf` function. |
| `core::warn("[EDIT] ...")` | `core::edit("...")` | Remove the manual `[EDIT] ` prefix; use the `edit` function. |
| `spdlog::info(...)` (direct) | `core::info(...)` | Go through the wrapper. |
| `spdlog::warn(...)` (direct) | `core::warn(...)` | Go through the wrapper. |
| `spdlog::error(...)` (direct) | `core::error(...)` | Go through the wrapper. |
| `spdlog::set_level(...)` | **Remove entirely** | Categories replace level filtering. |
| `using spdlog::critical;` | **Remove** | No `critical` in the new API. |

**Render-pathway logging in `perform_renderer.cpp`:** The existing
`core::info("Name: {}", name)`, `core::info("RenderPassID: {}", ...)`, etc. are
per-frame render-path prints. Migrate these to `core::render(...)`. Since these
run every frame and the user doesn't want per-frame spam, **wrap them in a
caller-side gate** (see Step 4 for the pattern). For now, just change `info` →
`render`; the gating happens in Step 4.

**Files to migrate (in order):**

1. `src/utils/editing.cpp` — ~20 call sites. All `core::warn("[EDIT] ...")` →
   `core::edit("...")`. The one `core::warn("editing: remove on empty cell...")`
   (line 259, no `[EDIT]` prefix) stays as `core::warn(...)`.
2. `src/utils/voxel_management.cpp` — ~10 call sites. `core::warn("[PERF] ...")`
   → `core::perf("...")`. `core::debug(...)` → `core::trace(...)` (line 173 is
   commented out — leave it or delete it). The `core::info("Time spent...")`
   lines (419, 435, 439) that use string concatenation instead of fmt — convert
   to fmt format: `core::perf("Time spent processing voxelBatchA: {}ms", loopElapsed)`.
3. `src/utils/compose_io.cpp` — ~25 call sites. Most are `core::info`/`core::error`/
   `core::warn` — no change needed except going through the new template. One
   `core::debug(...)` → `core::trace(...)` (line 387).
4. `src/utils/voxel_math.cpp` — 1 call site. `core::info(...)` → no change.
5. `src/graphics/gpu_interface.cpp` — ~18 call sites. All `core::warn("[PERF] ...")`
   → `core::perf("...")`. The rest are `core::info`/`core::error` — no change.
6. `src/graphics/perform_renderer.cpp` — ~10 call sites. `core::info(...)`
   → `core::render(...)`. See Step 4 for gating.
7. `src/graphics/disk_io.cpp` — 2 direct `spdlog::info(logFormat, ...)` calls
   (lines 149, 167). Convert to `core::info(logFormat, ...)`.
8. `src/core/ecs.cpp` — 3 call sites. `core::info(...)` — no change needed.
9. `docs/examples/editing_p1/main.cpp` — uses `projv::core::info(...)`,
   `projv::core::error(...)`, `projv::core::warn(...)`. No tag changes needed.
   Remove any `spdlog::set_level(...)` if present.
10. `docs/examples/edit_demo/main.cpp` — remove `spdlog::set_level(spdlog::level::warn)`
    (line 340). The `core::info(...)` calls stay as-is.
11. `docs/examples/PathTracer/main.cpp` — remove `spdlog::set_level(...)`.
    `core::info(...)` stays. `core::warn("PROFILING: ...")` → `core::perf("...")`
    (remove the `PROFILING: ` prefix since `[PRF]` is now automatic).
12. `docs/examples/ObjVoxelizer/main.cpp` — `core::error`/`core::warn`/`core::info`
    — no change needed.

**After migrating each file, build and check for errors:**
```bash
cmake --build build 2>&1 | grep -E "error:|warning:.*deprecated"
```

**Common pitfalls during migration:**
- `core::debug` and `core::critical` no longer exist. If a file uses them, it
  won't compile. Replace with `core::trace` and `core::error` respectively.
- The old `using spdlog::debug;` and `using spdlog::critical;` lines in
  `log.h` are gone. Any code that relied on them via `core::debug` will fail
  to compile — that's the intended signal to migrate.
- String concatenation in log args (`"foo " + std::to_string(x)`) works but is
  ugly. Where you see it, convert to fmt format (`"foo {}", x`).
- Make sure no manual `[TRC]`/`[PRF]`/`[EDT]`/`[RND]`/`[INF]`/`[WRN]`/`[ERR]`
  prefixes remain in format strings — the functions add them automatically.
  Double-tagging (`[PRF] [PRF] ...`) is the most common mistake.

### Step 4 — Gate per-frame render logging

**File:** `src/graphics/perform_renderer.cpp`

The render-path logging in this file runs every frame. The user wants:
- Render graph/structure: print **on startup or when it changes**, not every frame.
- Performance metrics (frame times): print **every ~100 frames** as an
  average/min/max summary, not every frame.

**Pattern A — Log on change (render graph/state):**

For the render pass enumeration (`Name: {}`, `RenderPassID: {}`, etc.), add a
static "last logged" generation counter and gate the whole block:

```cpp
#if defined(PROJV_ENABLE_RENDER)
    static size_t lastLoggedHash = 0;
    size_t currentHash = computeRenderGraphHash(renderer);  // or use a gen counter
    if (currentHash != lastLoggedHash) {
        lastLoggedHash = currentHash;
        core::render("=== Render graph changed ===");
        for (const auto& renderPass : renderer.renderPasses) {
            core::render("Name: {}", name);
            core::render("RenderPassID: {}", renderPass.renderPassID);
            // ... etc
        }
    }
#endif
```

If computing a hash is too complex, use a simpler "logged once" flag or a
generation counter that the renderer bumps when its structure changes. The
point is: don't call `core::render(...)` for static information every frame.

**Pattern B — Periodic perf summary (frame times):**

In the main render loop (wherever frame time is measured — likely in the
example `main.cpp` files or a future frame loop), accumulate frame times and
emit a summary every N frames:

```cpp
#if defined(PROJV_ENABLE_PERF)
    static int frameCount = 0;
    static double frameTimes[100];
    frameTimes[frameCount % 100] = frameDurationMs;
    if (++frameCount % 100 == 0) {
        double sum = 0, mn = 1e9, mx = 0;
        for (int i = 0; i < 100; i++) {
            sum += frameTimes[i];
            mn = std::min(mn, frameTimes[i]);
            mx = std::max(mx, frameTimes[i]);
        }
        core::perf("Frame stats (last 100): avg={:.2f}ms min={:.2f}ms max={:.2f}ms",
                   sum / 100.0, mn, mx);
    }
#endif
```

**Key principle:** The `#if defined(...)` guard around the **entire block**
(including the counter and stats) means that when PERF or RENDER is compiled
out, there is zero overhead — no counter, no array, no modulo, no min/max.
This is why the guard wraps the caller-side logic, not just the `core::perf`
call.

**Where to put Pattern B:** Frame-time measurement currently lives in
`docs/examples/PathTracer/main.cpp` and `docs/examples/edit_demo/main.cpp`
(the example entry points that have render loops). Since there's no shared
"frame loop" library yet, each example's `main.cpp` implements its own. When a
shared frame loop is added later, move the perf summary there.

### Step 5 — Update `CODING_STYLE.md`

**File:** `CODING_STYLE.md`

Find the existing logging examples (around line 274-276):
```cpp
core::info("Successfully created voxel at position ({}, {}, {})", x, y, z);
core::warn("Texture file not found, using default texture");
core::error("Failed to initialize GPU interface");
```

Update them to reflect the new category functions and the tag convention. Add
a short section explaining the logging system:

```markdown
## Logging

Use the category functions in `projv::core` (decled in `include/core/log.h`):

| Function | Tag  | Use for |
|----------|------|---------|
| `core::trace`  | `[TRC]` | Extremely detailed step-by-step tracing. Off in release. |
| `core::perf`   | `[PRF]` | Performance measurements and timing. |
| `core::edit`   | `[EDT]` | Editing pathway (voxel add/remove, chunk/grid ops). |
| `core::render` | `[RND]` | Rendering pathway (render passes, GPU state). |
| `core::info`   | `[INF]` | General informational messages. |
| `core::warn`   | `[WRN]` | Warnings — something unusual but recoverable. |
| `core::error`  | `[ERR]` | Errors — something failed. |

Tags are prepended automatically by the function. **Do not add them manually**
in format strings.

Do not print per-frame information unconditionally. Gate per-frame logging
with caller-side logic:

```cpp
#if defined(PROJV_ENABLE_RENDER)
    if (stateChanged) {
        core::render("Render graph: {} passes", passCount);
    }
#endif
```

For periodic perf summaries (every N frames), accumulate stats and emit on
a counter. Wrap the **entire block** (including stats) in `#if defined(...)` so
there is zero overhead when the category is disabled.

Categories are toggled at compile time via CMake:
```bash
cmake -DPROJV_LOG_TRACE=OFF -DPROJV_LOG_MINIMAL=ON ..
```
```

### Step 6 — Build and verify

```bash
# Full build with all categories on (default)
rm -rf build && cmake -S . -B build && cmake --build build

# Build with minimal logging (only WARN+ERROR)
rm -rf build && cmake -S . -B build -DPROJV_LOG_MINIMAL=ON && cmake --build build

# Build with trace off but perf on
rm -rf build && cmake -S . -B build -DPROJV_LOG_TRACE=OFF && cmake --build build
```

All three configurations must compile cleanly.

Run the existing test driver:
```bash
make -C docs/examples/editing_p1 clean && make -C docs/examples/editing_p1
./docs/examples/editing_p1/editing_p1
```
All existing assertions should still pass. The output should now show `[INF]`,
`[WRN]`, `[ERR]` tags on log lines.

### Step 7 — Update `AGENTS.md`

**File:** `AGENTS.md`

Add a section under "Delivered" documenting the logging system:

```markdown
### Delivered (Logging System)
- `include/core/log.h`: Replaced spdlog re-export with category-based template
  functions (`trace`, `perf`, `edit`, `render`, `info`, `warn`, `error`). Each
  gated by `#if defined(PROJV_ENABLE_*)` — empty body when disabled, eliminated
  by compiler. Tags (`[TRC]`, `[PRF]`, `[EDT]`, `[RND]`, `[INF]`, `[WRN]`,
  `[ERR]`) baked into functions.
- `CMakeLists.txt`: Seven `option()` calls (all ON by default) +
  `PROJV_LOG_MINIMAL` preset (disables everything except WARN+ERROR) +
  `add_compile_definitions` to emit defines project-wide.
- Migrated all existing call sites: manual `[PERF]`/`[EDIT]` tags removed,
  `core::debug` → `core::trace`, `core::critical` → `core::error`, direct
  `spdlog::` calls → `core::` wrapper, `spdlog::set_level()` calls removed.
- `perform_renderer.cpp`: Per-frame render logging gated with `#if` + on-change
  detection. Frame-time perf summary every 100 frames in example main loops.
- `CODING_STYLE.md`: Updated logging section with category table and gating
  guidance.
```

## File change summary

| File | Change |
|------|--------|
| `include/core/log.h` | **Rewrite.** Category template functions with compile-time toggles. |
| `CMakeLists.txt` | Add 7 `option()` calls + `PROJV_LOG_MINIMAL` preset + `add_compile_definitions`. |
| `src/utils/editing.cpp` | `core::warn("[EDIT] ...")` → `core::edit("...")` (~20 sites). |
| `src/utils/voxel_management.cpp` | `core::warn("[PERF] ...")` → `core::perf("...")`, `core::debug` → `core::trace`, string concat → fmt. |
| `src/utils/compose_io.cpp` | `core::debug` → `core::trace` (1 site). Rest unchanged. |
| `src/utils/voxel_math.cpp` | No change needed. |
| `src/graphics/gpu_interface.cpp` | `core::warn("[PERF] ...")` → `core::perf("...")` (~18 sites). |
| `src/graphics/perform_renderer.cpp` | `core::info(...)` → `core::render(...)`. Add on-change gating. |
| `src/graphics/disk_io.cpp` | `spdlog::info(...)` → `core::info(...)` (2 sites). |
| `src/core/ecs.cpp` | No change needed. |
| `docs/examples/editing_p1/main.cpp` | Remove `spdlog::set_level` if present. |
| `docs/examples/edit_demo/main.cpp` | Remove `spdlog::set_level` (line 340). Add frame-time perf summary. |
| `docs/examples/PathTracer/main.cpp` | Remove `spdlog::set_level`. `core::warn("PROFILING: ...")` → `core::perf("...")`. Add frame-time perf summary. |
| `docs/examples/ObjVoxelizer/main.cpp` | No change needed. |
| `CODING_STYLE.md` | Update logging section. |
| `AGENTS.md` | Add logging system delivery entry. |

## Definition of done

- `include/core/log.h` defines 7 category functions, each gated by
  `#if defined(PROJV_ENABLE_*)`.
- All 7 CMake `option()` calls exist, all default ON.
- `PROJV_LOG_MINIMAL=ON` disables TRACE, PERF, EDIT, RENDER, INFO — only
  WARN and ERROR remain.
- No manual `[PERF]`, `[EDIT]`, `[TRC]`, `[PRF]`, `[EDT]`, `[RND]`, `[INF]`,
  `[WRN]`, `[ERR]` tags appear in any format string in `src/` or
  `docs/examples/`.
- No `spdlog::` direct calls remain in `src/` (external/ submodule excluded).
- No `spdlog::set_level(...)` calls remain anywhere in the project.
- `core::debug` and `core::critical` are not used anywhere.
- Full build succeeds with all categories ON.
- Full build succeeds with `PROJV_LOG_MINIMAL=ON`.
- `editing_p1` test driver passes all existing assertions.
- Log output lines contain `[XXX]` tags visible in stderr.
- Per-frame render logging in `perform_renderer.cpp` is gated (does not print
  every frame for static information).

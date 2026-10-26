# Logging

Seven categories in `projv::core`, each compiled in or out.
Header: [`core/log.h`](../../include/core/log.h). Design: [plans/logging.md](../plans/logging.md).

```cpp
#include "core/log.h"

projv::core::info("Loaded {} chunk(s) from {}", count, path);       // fmt syntax
projv::core::warn("palette full on '{}'", name);
projv::core::perf("flush took {:.2f} ms", ms);
```

| Function | Tag | spdlog level | CMake switch |
|---|---|---|---|
| `trace` | `[TRC]` | trace | `PROJV_LOG_TRACE` |
| `perf` | `[PRF]` | info | `PROJV_LOG_PERF` |
| `edit` | `[EDT]` | info | `PROJV_LOG_EDIT` |
| `render` | `[RND]` | info | `PROJV_LOG_RENDER` |
| `info` | `[INF]` | info | `PROJV_LOG_INFO` |
| `warn` | `[WRN]` | warn | `PROJV_LOG_WARN` |
| `error` | `[ERR]` | error | `PROJV_LOG_ERROR` |


```bash
./sandbox 2>&1 | grep -E '\[(WRN|ERR)\]'
./terrain_generator 2>&1 | grep '\[PRF\]'
```

`trace` is hidden at spdlog's default level: `spdlog::set_level(spdlog::level::trace)`.

## Rate-limited logging

Each call site keeps its own counter or clock:

```cpp
core_perf_every(100, "frame {:.2f} ms", ms);                 // every 100th call
core_warn_every_ms(1000, "{} commands late", late);          // at most once a second
```

Every category has `core_<category>_every(N, ...)` and `core_<category>_every_ms(MS, ...)`.

## Compiling categories out

A disabled category's calls compile to nothing.

```bash
cmake --preset dev -DPROJV_LOG_TRACE=OFF -DPROJV_LOG_EDIT=OFF
cmake --preset release                         # PROJV_LOG_MINIMAL=ON: WARN and ERROR only
```

- **`PROJV_LOG_MINIMAL` forces the optional categories off in the cache.** Turning it back off does
  not restore them; set them `ON` explicitly.
- **The switches are `PUBLIC`** (`PROJV_ENABLE_<CATEGORY>` defines), so consumers inherit the
  library's set. A different set means different inline code, not just different output.
- **Guard bookkeeping that only feeds a log line:**

```cpp
#if defined(PROJV_ENABLE_PERF)
    static double frameTimes[100];
    // ...
#endif
```

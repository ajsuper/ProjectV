#ifndef PROJV_CORE_LOG_H
#define PROJV_CORE_LOG_H

#include "spdlog/spdlog.h"
#include "spdlog/fmt/fmt.h"
#include <chrono>

namespace projv::core {

// ===== TRACE =====
#if defined(PROJV_ENABLE_TRACE)
template <typename... Args>
inline void trace(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::trace("[TRC] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#define core_trace_every(N, fmt, ...) do { \
    static int _core_trace_cnt = 0; \
    if (++_core_trace_cnt % (N) == 0) \
        projv::core::trace(fmt, ##__VA_ARGS__); \
} while(0)
#define core_trace_every_ms(MS, fmt, ...) do { \
    static auto _core_trace_last = std::chrono::steady_clock::now(); \
    auto _core_trace_now = std::chrono::steady_clock::now(); \
    if (std::chrono::duration<double, std::milli>(_core_trace_now - _core_trace_last).count() >= (MS)) { \
        _core_trace_last = _core_trace_now; \
        projv::core::trace(fmt, ##__VA_ARGS__); \
    } \
} while(0)
#else
template <typename... Args>
inline void trace(fmt::format_string<Args...>, Args&&...) {}
#define core_trace_every(N, fmt, ...) do {} while(0)
#define core_trace_every_ms(MS, fmt, ...) do {} while(0)
#endif

// ===== PERF =====
#if defined(PROJV_ENABLE_PERF)
template <typename... Args>
inline void perf(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::info("[PRF] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#define core_perf_every(N, fmt, ...) do { \
    static int _core_perf_cnt = 0; \
    if (++_core_perf_cnt % (N) == 0) \
        projv::core::perf(fmt, ##__VA_ARGS__); \
} while(0)
#define core_perf_every_ms(MS, fmt, ...) do { \
    static auto _core_perf_last = std::chrono::steady_clock::now(); \
    auto _core_perf_now = std::chrono::steady_clock::now(); \
    if (std::chrono::duration<double, std::milli>(_core_perf_now - _core_perf_last).count() >= (MS)) { \
        _core_perf_last = _core_perf_now; \
        projv::core::perf(fmt, ##__VA_ARGS__); \
    } \
} while(0)
#else
template <typename... Args>
inline void perf(fmt::format_string<Args...>, Args&&...) {}
#define core_perf_every(N, fmt, ...) do {} while(0)
#define core_perf_every_ms(MS, fmt, ...) do {} while(0)
#endif

// ===== EDIT =====
#if defined(PROJV_ENABLE_EDIT)
template <typename... Args>
inline void edit(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::info("[EDT] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#define core_edit_every(N, fmt, ...) do { \
    static int _core_edit_cnt = 0; \
    if (++_core_edit_cnt % (N) == 0) \
        projv::core::edit(fmt, ##__VA_ARGS__); \
} while(0)
#define core_edit_every_ms(MS, fmt, ...) do { \
    static auto _core_edit_last = std::chrono::steady_clock::now(); \
    auto _core_edit_now = std::chrono::steady_clock::now(); \
    if (std::chrono::duration<double, std::milli>(_core_edit_now - _core_edit_last).count() >= (MS)) { \
        _core_edit_last = _core_edit_now; \
        projv::core::edit(fmt, ##__VA_ARGS__); \
    } \
} while(0)
#else
template <typename... Args>
inline void edit(fmt::format_string<Args...>, Args&&...) {}
#define core_edit_every(N, fmt, ...) do {} while(0)
#define core_edit_every_ms(MS, fmt, ...) do {} while(0)
#endif

// ===== RENDER =====
#if defined(PROJV_ENABLE_RENDER)
template <typename... Args>
inline void render(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::info("[RND] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#define core_render_every(N, fmt, ...) do { \
    static int _core_render_cnt = 0; \
    if (++_core_render_cnt % (N) == 0) \
        projv::core::render(fmt, ##__VA_ARGS__); \
} while(0)
#define core_render_every_ms(MS, fmt, ...) do { \
    static auto _core_render_last = std::chrono::steady_clock::now(); \
    auto _core_render_now = std::chrono::steady_clock::now(); \
    if (std::chrono::duration<double, std::milli>(_core_render_now - _core_render_last).count() >= (MS)) { \
        _core_render_last = _core_render_now; \
        projv::core::render(fmt, ##__VA_ARGS__); \
    } \
} while(0)
#else
template <typename... Args>
inline void render(fmt::format_string<Args...>, Args&&...) {}
#define core_render_every(N, fmt, ...) do {} while(0)
#define core_render_every_ms(MS, fmt, ...) do {} while(0)
#endif

// ===== INFO =====
#if defined(PROJV_ENABLE_INFO)
template <typename... Args>
inline void info(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::info("[INF] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#define core_info_every(N, fmt, ...) do { \
    static int _core_info_cnt = 0; \
    if (++_core_info_cnt % (N) == 0) \
        projv::core::info(fmt, ##__VA_ARGS__); \
} while(0)
#define core_info_every_ms(MS, fmt, ...) do { \
    static auto _core_info_last = std::chrono::steady_clock::now(); \
    auto _core_info_now = std::chrono::steady_clock::now(); \
    if (std::chrono::duration<double, std::milli>(_core_info_now - _core_info_last).count() >= (MS)) { \
        _core_info_last = _core_info_now; \
        projv::core::info(fmt, ##__VA_ARGS__); \
    } \
} while(0)
#else
template <typename... Args>
inline void info(fmt::format_string<Args...>, Args&&...) {}
#define core_info_every(N, fmt, ...) do {} while(0)
#define core_info_every_ms(MS, fmt, ...) do {} while(0)
#endif

// ===== WARN =====
#if defined(PROJV_ENABLE_WARN)
template <typename... Args>
inline void warn(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::warn("[WRN] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#define core_warn_every(N, fmt, ...) do { \
    static int _core_warn_cnt = 0; \
    if (++_core_warn_cnt % (N) == 0) \
        projv::core::warn(fmt, ##__VA_ARGS__); \
} while(0)
#define core_warn_every_ms(MS, fmt, ...) do { \
    static auto _core_warn_last = std::chrono::steady_clock::now(); \
    auto _core_warn_now = std::chrono::steady_clock::now(); \
    if (std::chrono::duration<double, std::milli>(_core_warn_now - _core_warn_last).count() >= (MS)) { \
        _core_warn_last = _core_warn_now; \
        projv::core::warn(fmt, ##__VA_ARGS__); \
    } \
} while(0)
#else
template <typename... Args>
inline void warn(fmt::format_string<Args...>, Args&&...) {}
#define core_warn_every(N, fmt, ...) do {} while(0)
#define core_warn_every_ms(MS, fmt, ...) do {} while(0)
#endif

// ===== ERROR =====
#if defined(PROJV_ENABLE_ERROR)
template <typename... Args>
inline void error(fmt::format_string<Args...> fmt_str, Args&&... args) {
    spdlog::error("[ERR] {}", fmt::format(fmt_str, std::forward<Args>(args)...));
}
#define core_error_every(N, fmt, ...) do { \
    static int _core_error_cnt = 0; \
    if (++_core_error_cnt % (N) == 0) \
        projv::core::error(fmt, ##__VA_ARGS__); \
} while(0)
#define core_error_every_ms(MS, fmt, ...) do { \
    static auto _core_error_last = std::chrono::steady_clock::now(); \
    auto _core_error_now = std::chrono::steady_clock::now(); \
    if (std::chrono::duration<double, std::milli>(_core_error_now - _core_error_last).count() >= (MS)) { \
        _core_error_last = _core_error_now; \
        projv::core::error(fmt, ##__VA_ARGS__); \
    } \
} while(0)
#else
template <typename... Args>
inline void error(fmt::format_string<Args...>, Args&&...) {}
#define core_error_every(N, fmt, ...) do {} while(0)
#define core_error_every_ms(MS, fmt, ...) do {} while(0)
#endif

} // namespace projv::core
#endif // PROJV_CORE_LOG_H
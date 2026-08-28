cmake_minimum_required(VERSION 3.20) # bgfx.cmake requires 3.20.
# C as well as C++: several dependencies and one example bring C sources -- bgfx's 3rdparty,
# Assimp's bundled zlib, and Lua, which the SceneEditor's brush system embeds.
project(ProjectV VERSION 0.1.0 LANGUAGES C CXX)

# ProjectV builds as one static library, `projectV`, exported as `ProjectV::projectV`.
#
# Every dependency is resolved the same way: look for an installed config package first
# (which is what vcpkg, a system install, or a superbuild provides), and fall back to the
# in-tree submodule. That means the vcpkg build and the submodule build take the *same*
# code path through this file rather than being two separate build systems that drift.

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build type only matters for single-config generators, where an unset one silently means
# "no optimisation flags at all" -- which used to be masked by -O3 being hard-appended to
# CMAKE_CXX_FLAGS for every build, debug included.
get_property(PROJV_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT PROJV_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Archives land in the build tree. They used to be written to an in-source lib/, which is
# how that directory accumulated archives for targets that no longer existed and handed
# stale objects to anything linking -L lib.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(PROJV_BUILD_EXAMPLES "Build the bundled examples" OFF)
option(PROJV_INSTALL        "Generate the install and export rules" ON)

# --- Logging categories (all ON by default) -----------------------------------------
option(PROJV_LOG_TRACE  "Enable trace logging"             ON)
option(PROJV_LOG_PERF   "Enable performance logging"        ON)
option(PROJV_LOG_EDIT   "Enable editing pathway logging"    ON)
option(PROJV_LOG_RENDER "Enable rendering pathway logging"  ON)
option(PROJV_LOG_INFO   "Enable info logging"               ON)
option(PROJV_LOG_WARN   "Enable warning logging"            ON)
option(PROJV_LOG_ERROR  "Enable error logging"              ON)

# Preset: disable everything except WARN+ERROR for release/production builds.
# Usage: cmake -DPROJV_LOG_MINIMAL=ON ..
option(PROJV_LOG_MINIMAL "Disable all optional logging (keep only WARN+ERROR)" OFF)
if(PROJV_LOG_MINIMAL)
    foreach(category TRACE PERF EDIT RENDER INFO)
        set(PROJV_LOG_${category} OFF CACHE BOOL "" FORCE)
    endforeach()
endif()

set(PROJV_LOG_DEFS "")
foreach(category TRACE PERF EDIT RENDER INFO WARN ERROR)
    if(PROJV_LOG_${category})
        list(APPEND PROJV_LOG_DEFS PROJV_ENABLE_${category})
    endif()
endforeach()
message(STATUS "ProjectV logging categories: ${PROJV_LOG_DEFS}")

# --- Linux GLFW native backend: Wayland (default) or X11 -----------------------------
# This is PUBLIC on the target below rather than a directory-scope define. render_instance.h
# selects GLFW_EXPOSE_NATIVE_WAYLAND or _X11 from it, so a consumer that compiled without it
# would pick a different native handle than the library it links against.
option(PROJV_USE_X11 "Use X11 backend for GLFW on Linux (OFF = Wayland)" OFF)
if(PROJV_USE_X11)
    message(STATUS "GLFW Linux backend: X11")
else()
    message(STATUS "GLFW Linux backend: Wayland")
endif()

# =====================================================================================
# Dependencies
# =====================================================================================

# bgfx. The config package is what vcpkg's `bgfx` port installs; the submodule fallback is
# bgfx.cmake, which is what that port is *built from*, so both give the same targets and the
# same bgfx_compile_shaders. This replaced an ExternalProject that shelled out to bgfx's GENie
# makefile and then declared IMPORTED targets whose location was hardcoded to a macOS path on
# every platform -- so the imported bgfx target had never been correct on Linux.
find_package(bgfx CONFIG QUIET)
if(bgfx_FOUND)
    message(STATUS "bgfx: found installed config package")
else()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/bgfx.cmake/CMakeLists.txt")
        message(FATAL_ERROR
            "bgfx was not found and external/bgfx.cmake is missing.\n"
            "Either install bgfx (vcpkg: `vcpkg install bgfx[tools]`) or check out the "
            "submodules: git submodule update --init --recursive")
    endif()
    message(STATUS "bgfx: building from external/bgfx.cmake")
    set(PROJV_VENDORED_DEPS ON)
    # Point bgfx.cmake at the bgfx/bx/bimg submodules this repo already pins, rather than at
    # its own nested copies, so the engine keeps building against the revisions it was
    # developed against.
    set(BGFX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/bgfx" CACHE STRING "" FORCE)
    set(BX_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/external/bx"   CACHE STRING "" FORCE)
    set(BIMG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/bimg" CACHE STRING "" FORCE)
    set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE) # ~30 sample apps we never run.
    set(BGFX_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    # shaderc only. The texture and geometry viewers link bgfx's example-common, which drags in
    # the sample framework this build has no use for and adds minutes to a cold build.
    set(BGFX_BUILD_TOOLS          ON  CACHE BOOL "" FORCE)
    set(BGFX_BUILD_TOOLS_SHADER   ON  CACHE BOOL "" FORCE)
    set(BGFX_BUILD_TOOLS_BIN2C    OFF CACHE BOOL "" FORCE)
    set(BGFX_BUILD_TOOLS_GEOMETRY OFF CACHE BOOL "" FORCE)
    set(BGFX_BUILD_TOOLS_TEXTURE  OFF CACHE BOOL "" FORCE)
    set(BGFX_BUILD_EXAMPLE_COMMON OFF CACHE BOOL "" FORCE)
    set(BGFX_INSTALL        ${PROJV_INSTALL} CACHE BOOL "" FORCE)
    add_subdirectory(external/bgfx.cmake)
endif()

# bgfx.cmake names its libraries `bgfx`, `bx` and `bimg` with no namespace; the installed
# config package names them `bgfx::bgfx` and so on. Normalise to the namespaced spelling so
# the rest of this file does not care which one is in play.
foreach(lib bgfx bx bimg)
    if(NOT TARGET bgfx::${lib} AND TARGET ${lib})
        add_library(bgfx::${lib} ALIAS ${lib})
    endif()
endforeach()
if(NOT TARGET bgfx::bgfx)
    message(FATAL_ERROR "bgfx was resolved but no usable bgfx target was produced.")
endif()

# glm / spdlog / nlohmann_json / glfw3: same find-then-fallback shape.
# Each vendored dependency installs its own config package into the same prefix, so the
# find_dependency() calls in ProjectVConfig.cmake resolve there. Without this the export set
# would name targets that exist nowhere in the install tree.
find_package(glm CONFIG QUIET)
if(NOT glm_FOUND)
    set(GLM_BUILD_INSTALL ${PROJV_INSTALL} CACHE BOOL "" FORCE)
    add_subdirectory(external/glm)
    set(PROJV_VENDORED_DEPS ON)
endif()

find_package(spdlog CONFIG QUIET)
if(NOT spdlog_FOUND)
    set(SPDLOG_INSTALL ${PROJV_INSTALL} CACHE BOOL "" FORCE)
    add_subdirectory(external/spdlog)
    set(PROJV_VENDORED_DEPS ON)
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    set(JSON_Install ${PROJV_INSTALL} CACHE BOOL "" FORCE)
    add_subdirectory(external/json)
    set(PROJV_VENDORED_DEPS ON)
endif()

# GLFW has no submodule; it is a system package on every platform ProjectV supports.
find_package(glfw3 CONFIG QUIET)
if(glfw3_FOUND)
    set(PROJV_GLFW_TARGET glfw)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GLFW3 QUIET IMPORTED_TARGET glfw3)
    endif()
    if(TARGET PkgConfig::GLFW3)
        set(PROJV_GLFW_TARGET PkgConfig::GLFW3)
    else()
        message(STATUS "glfw3: no config or pkg-config module found, falling back to -lglfw")
        set(PROJV_GLFW_TARGET glfw)
    endif()
endif()

# =====================================================================================
# The library
# =====================================================================================

# One target. There used to be sixteen, one per .cpp, which no consumer ever linked a subset
# of -- its only real effect was that compose_io/voxel_management/material reference each
# other, so every caller had to wrap the archives in -Wl,--start-group and get the order
# right. Four of the seven example Makefiles did not, and worked only by luck.
add_library(projectV STATIC
    src/core/ecs.cpp
    src/core/math.cpp
    src/core/paths.cpp

    src/graphics/disk_io.cpp
    src/graphics/gpu_interface.cpp
    src/graphics/manage_resources.cpp
    src/graphics/perform_renderer.cpp
    src/graphics/render_instance.cpp
    src/graphics/type_mapping.cpp

    src/utils/animation.cpp
    src/utils/compose_io.cpp
    src/utils/editing.cpp
    src/utils/material.cpp
    src/utils/picking.cpp
    src/utils/scene_query.cpp
    src/utils/voxel_management.cpp
    src/utils/voxel_math.cpp
)
add_library(ProjectV::projectV ALIAS projectV)

# PUBLIC, so consumers inherit the include path. These were PRIVATE, which is exactly why
# every example Makefile had to re-list the same eight -I flags by hand.
#
# Headers install under <prefix>/include/projv/, and the install interface points *at* that
# directory rather than at its parent. So `#include "core/ecs.h"` keeps working unchanged
# while the install root stays free of a top-level core/, utils/ and data_structures/.
target_include_directories(projectV PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/projv>
)

target_compile_features(projectV PUBLIC cxx_std_17)

# PUBLIC. The log macros compile their call sites out, so a consumer built with a different
# set than the library silently gets different behaviour -- which is the situation the
# example Makefiles were in, each defining its own -DPROJV_ENABLE_* by hand.
target_compile_definitions(projectV PUBLIC ${PROJV_LOG_DEFS})
if(PROJV_USE_X11)
    target_compile_definitions(projectV PUBLIC PROJV_USE_X11)
endif()

target_compile_options(projectV PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# glm and spdlog are used header-only, which is how they were used before the build was
# consolidated -- no example ever linked either. It matters more than it looks: spdlog's compiled
# target puts SPDLOG_COMPILED_LIB on its PUBLIC interface, so linking it would make libspdlog a
# hard requirement of every consumer of ProjectV, and of the installed package.
if(TARGET glm::glm-header-only)
    set(PROJV_GLM_TARGET glm::glm-header-only)
else()
    set(PROJV_GLM_TARGET glm::glm)
endif()
if(TARGET spdlog::spdlog_header_only)
    set(PROJV_SPDLOG_TARGET spdlog::spdlog_header_only)
else()
    set(PROJV_SPDLOG_TARGET spdlog::spdlog)
endif()

# All PUBLIC: every one of these appears in the public headers. bgfx and GLFW in
# render_instance.h and the data_structures, glm through core/math.h, spdlog through
# core/log.h, and nlohmann_json through graphics/disk_io.h.
target_link_libraries(projectV PUBLIC
    bgfx::bgfx
    bgfx::bx
    bgfx::bimg
    ${PROJV_GLM_TARGET}
    ${PROJV_SPDLOG_TARGET}
    nlohmann_json::nlohmann_json
    ${PROJV_GLFW_TARGET}
)

if(APPLE)
    target_link_libraries(projectV PUBLIC
        "-framework Cocoa" "-framework Metal" "-framework IOKit" "-framework QuartzCore")
endif()

include(ProjectVShaders)

if(PROJV_BUILD_EXAMPLES)
    add_subdirectory(docs/examples)
endif()

# =====================================================================================
# Install / export
# =====================================================================================

# An install whose dependencies came from external/ is self-contained: each vendored project
# installs its own config package into the same prefix, and ProjectVConfig finds them there.
# That works, but it means the prefix carries ProjectV's copies of bgfx, glm, spdlog and
# nlohmann_json -- so install it somewhere of its own rather than over a prefix that already
# has those from another source. A vcpkg build has no such problem: every dependency comes
# from find_package and nothing extra is installed.
if(PROJV_INSTALL AND PROJV_VENDORED_DEPS)
    message(STATUS
        "ProjectV: installing will also install the vendored dependencies (they are built from "
        "external/ rather than found as packages). Use a dedicated install prefix, or configure "
        "with the vcpkg preset for an install that carries only ProjectV.")
endif()

if(PROJV_INSTALL)
    install(TARGETS projectV
        EXPORT ProjectVTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # Headers, plus pjv_utils_DDA.sc -- the engine's shader-side voxel traversal. It is a
    # shader include living in the C++ include tree, and it has to ship: every renderer in
    # the tree includes it, so shaderc needs <prefix>/include/projv on its -i path.
    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/projv
        FILES_MATCHING
            PATTERN "*.h"
            PATTERN "*.sc"
    )

    install(EXPORT ProjectVTargets
        FILE ProjectVTargets.cmake
        NAMESPACE ProjectV::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ProjectV
    )

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProjectVConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/ProjectVConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ProjectV
    )
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ProjectVConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ProjectVConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ProjectVConfigVersion.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ProjectVShaders.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ProjectV
    )
endif()

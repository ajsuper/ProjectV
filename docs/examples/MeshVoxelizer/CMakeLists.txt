cmake_minimum_required(VERSION 3.16)
project(projectV_voxelizer CXX)

# The voxelizer is built with CMake rather than a hand-written Makefile because Assimp — the
# multi-format mesh importer this tool leans on — is a CMake project. Building it as a subproject is
# the only mesh-import story that is identical on Linux, macOS, and Windows; system packages differ
# per distro and do not exist at all on Windows. `make` in this directory forwards here.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Root of the ProjectV checkout (this example lives at docs/examples/MeshVoxelizer).
get_filename_component(PROJECTV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

set(ASSIMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/assimp")
if(NOT EXISTS "${ASSIMP_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "external/assimp is empty — the Assimp submodule has not been checked out.\n"
        "Run: git submodule update --init --recursive docs/examples/MeshVoxelizer/external/assimp")
endif()

# --- Assimp -------------------------------------------------------------------------------------
# Built static and stripped to what a voxelizer needs: importers only (nothing is ever exported),
# no tools, no samples, no tests. The bundled zlib is used so nothing has to be installed system
# wide — several container formats (glTF-binary, compressed FBX, Blender) need it.
set(BUILD_SHARED_LIBS            OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS           OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES         OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS    OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_VIEW     OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL               OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT             ON  CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS    OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB            ON  CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX  OFF CACHE BOOL "" FORCE)

add_subdirectory(${ASSIMP_DIR} ${CMAKE_BINARY_DIR}/assimp EXCLUDE_FROM_ALL)

# --- Engine libraries ---------------------------------------------------------------------------
# The voxelizer links the prebuilt static libraries from the engine's top-level build. They are
# looked up rather than rebuilt so this example stays independent of the engine's build tree.
#
# Order matters: these are static archives, each scanned once, so a library must follow everything
# that references it. This is the same order the rendering examples use, minus the graphics libraries
# the voxelizer has no use for -- it writes a .data container and never opens a window.
#
# `ecs` and `scene_query` are here for compose_io's benefit rather than the voxelizer's own: it now
# builds its scenes through addComponent/setComponentTransform, so linking compose_io drags them in
# even though nothing in main.cpp calls either.
set(PROJECTV_LIBS
    projectV-ecs
    projectV-compose_io
    projectV-voxel_management
    projectV-material
    projectV-voxel_math
    projectV-math
    projectV-scene_query
)

foreach(lib ${PROJECTV_LIBS})
    find_library(${lib}_PATH NAMES ${lib} PATHS "${PROJECTV_DIR}/lib" NO_DEFAULT_PATH)
    if(NOT ${lib}_PATH)
        message(FATAL_ERROR
            "Could not find lib${lib}.a in ${PROJECTV_DIR}/lib.\n"
            "Build ProjectV first: cd ${PROJECTV_DIR} && cmake -B build && cmake --build build")
    endif()
    list(APPEND PROJECTV_LIB_PATHS ${${lib}_PATH})
endforeach()

# --- The tool -----------------------------------------------------------------------------------
add_executable(mesh_voxelizer main.cpp)

# Matches the warning level the engine builds at. Applied only to this target, so Assimp's own
# warnings do not drown it out.
if(NOT MSVC)
    target_compile_options(mesh_voxelizer PRIVATE -Wall -Wextra)
endif()

target_include_directories(mesh_voxelizer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECTV_DIR}/include
    ${PROJECTV_DIR}/external/spdlog/include
    ${PROJECTV_DIR}/external/json/include
    ${PROJECTV_DIR}/external/glm
    # core/math.h pulls in bx/math.h; no bgfx library is linked, only headers are needed.
    ${PROJECTV_DIR}/external/bx/include
    ${PROJECTV_DIR}/external/bgfx/include
    ${PROJECTV_DIR}/external/bimg/include
    # zlib, for the compressed NBT inside Minecraft region files. Assimp already vendors and builds
    # it, so this costs no additional dependency. zconf.h is generated into the build tree.
    ${ASSIMP_DIR}/contrib/zlib
    ${CMAKE_BINARY_DIR}/assimp/contrib/zlib
)

# The log wrapper compiles each level out unless its macro is defined; this tool's entire
# user-facing output goes through info/warn/error, so all three have to be on.
target_compile_definitions(mesh_voxelizer PRIVATE
    PROJV_ENABLE_INFO
    PROJV_ENABLE_WARN
    PROJV_ENABLE_ERROR
)

target_link_libraries(mesh_voxelizer PRIVATE assimp zlibstatic ${PROJECTV_LIB_PATHS})

# Drop the binary next to the sources, where the README and the terrain_generator example expect it.
set_target_properties(mesh_voxelizer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}"
)
